// Run statistics: one row per started run in speedbraten.db (SQLite) in the home dir, written
// when the run ends. Runs with state loads (splices > 0) are a separate category from normal
// runs. Nothing here reads or changes simulation state beyond what the finished run reports.

#include "game.h"
#include "inputs.h"
#ifndef HEADLESS
#include "sqlite3.h"
#include "blake3.h"
#include "replayio.h"
#endif

namespace game
{
#ifdef HEADLESS
    void statsrecord(const inputrun &run, const replayinfo &info, const char *replay, const uchar *content) {}
    void statsreplaymoved(const char *from, const char *to) {}
    bool statspbreplay(const char *map, const char *kind, int mode, bool segmented, char *file) { return false; }
    void statspbreplays(vector<char *> &files) {}
    bool statssplitcompare(const char *map, const char *kind, int mode, bool segmented, vector<splittime> &pb, vector<splitsegment> &best, int &pbms) { pbms = -1; return false; }
    int statsversion() { return 0; }
    bool statshistory(uchar *head, int &count, int &attempts) { return false; }
    bool statsghostchoice(const char *map, const char *kind, int mode, char *source, char *file, char *label) { return false; }
    bool statssplitsofreplay(const char *file, vector<splittime> &out) { return false; }
    void statssetghost(const char *map, const char *kind, int mode, const char *source, const char *file, const char *label) {}
#else
    static int changes = 0;
    // changes whenever a run is added, so callers can tell when a PB may have changed
    int statsversion() { return changes; }

    static sqlite3 *db = NULL;
    static bool dbfailed = false;

    static const char *SCHEMA =
        "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value);"
        "INSERT OR IGNORE INTO meta VALUES('schema', 1);"
        "CREATE TABLE IF NOT EXISTS attempts("
        "  id INTEGER PRIMARY KEY,"
        "  date INTEGER NOT NULL,"   // unix time when the run ended
        "  map TEXT NOT NULL,"       // first map of the run
        "  mode INTEGER NOT NULL,"
        "  kind TEXT NOT NULL,"      // sp, flagrun or race
        "  finished INTEGER NOT NULL,"
        "  runtime INTEGER,"         // ms on the run timer, NULL unless finished
        "  ticks INTEGER NOT NULL,"  // ticks played
        "  splices INTEGER NOT NULL,"
        "  maps INTEGER NOT NULL,"
        "  mapcrc INTEGER, cfgcrc INTEGER,"
        "  idle INTEGER NOT NULL,"   // longest stretch of ticks without an input event
        "  replay TEXT);"            // replay file, NULL without one
        "CREATE INDEX IF NOT EXISTS attempts_map ON attempts(map, kind, finished);"
        // the splits of a run in the order they fired; prev is the split before ('' at the start)
        // and segment the ticks since it. A finished run ends with a split named finish.
        "CREATE TABLE IF NOT EXISTS splits("
        "  attempt INTEGER NOT NULL REFERENCES attempts(id),"
        "  idx INTEGER NOT NULL, name TEXT NOT NULL, prev TEXT NOT NULL,"
        "  tick INTEGER NOT NULL, segment INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS splits_attempt ON splits(attempt);"
        // the ghost raced on a map, kind and mode when it is not the PB
        "CREATE TABLE IF NOT EXISTS ghosts("
        "  map TEXT NOT NULL, kind TEXT NOT NULL, mode INTEGER NOT NULL,"
        "  source TEXT NOT NULL, file TEXT, label TEXT,"
        "  PRIMARY KEY(map, kind, mode));";

    static bool dbexec(const char *sql)
    {
        char *err = NULL;
        if(sqlite3_exec(db, sql, NULL, NULL, &err) == SQLITE_OK) return true;
        conoutf(CON_ERROR, "stats: %s", err ? err : sqlite3_errmsg(db));
        sqlite3_free(err);
        return false;
    }

    static sqlite3 *statsdb()
    {
        if(db || dbfailed) return db;
        const char *file = findfile("speedbraten.db", "w");
        if(sqlite3_open_v2(file, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK)
        {
            conoutf(CON_ERROR, "stats: could not open %s: %s", file, db ? sqlite3_errmsg(db) : "out of memory");
            sqlite3_close(db);
            db = NULL;
            dbfailed = true;
            return NULL;
        }
        sqlite3_busy_timeout(db, 1000);
        if(!dbexec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;") || !dbexec(SCHEMA))
        {
            sqlite3_close(db);
            db = NULL;
            dbfailed = true;
        }
        // databases from before the content column: their history chain used another summary
        // layout, so it starts over
        else
        {
            if(sqlite3_exec(db, "ALTER TABLE attempts ADD COLUMN content BLOB", NULL, NULL, NULL) == SQLITE_OK)
                dbexec("DELETE FROM meta WHERE key IN ('historyhead', 'historycount')");
            // a run with gamespeed, pause or fixedframe never counts as a PB or best segment
            sqlite3_exec(db, "ALTER TABLE attempts ADD COLUMN assisted INTEGER NOT NULL DEFAULT 0", NULL, NULL, NULL);
        }
        return db;
    }

    static sqlite3_stmt *prepare(const char *sql)
    {
        if(!statsdb()) return NULL;
        sqlite3_stmt *st = NULL;
        if(sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        {
            conoutf(CON_ERROR, "stats: %s", sqlite3_errmsg(db));
            return NULL;
        }
        return st;
    }

    static void bindtext(sqlite3_stmt *st, int i, const char *s)
    {
        if(s) sqlite3_bind_text(st, i, s, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(st, i);
    }

    static const char *runkindname(const replayinfo &info)
    {
        return info.race ? "race" : info.flagrun ? "flagrun" : "sp";
    }

    static void recordsplits(sqlite3_int64 attempt, const replayinfo &info)
    {
        vector<splittime> times;
        splitresults(times);
        if(info.finished)
        {
            splittime &f = times.add();
            copystring(f.name, "finish");
            f.tick = info.runtime / SIMTICK;
        }
        if(times.empty()) return;
        sqlite3_stmt *st = prepare("INSERT INTO splits(attempt, idx, name, prev, tick, segment) VALUES(?, ?, ?, ?, ?, ?)");
        if(!st) return;
        dbexec("BEGIN");
        loopv(times)
        {
            sqlite3_bind_int64(st, 1, attempt);
            sqlite3_bind_int(st, 2, i);
            bindtext(st, 3, times[i].name);
            bindtext(st, 4, i ? times[i-1].name : "");
            sqlite3_bind_int(st, 5, times[i].tick);
            sqlite3_bind_int(st, 6, times[i].tick - (i ? times[i-1].tick : 0));
            sqlite3_step(st);
            sqlite3_reset(st);
        }
        dbexec("COMMIT");
        sqlite3_finalize(st);
    }

    static void extendhistory(const replayinfo &info, const uchar *content);

    void statsrecord(const inputrun &run, const replayinfo &info, const char *replay, const uchar *content)
    {
        sqlite3_stmt *st = prepare("INSERT INTO attempts(date, map, mode, kind, finished, runtime, ticks, splices, maps, mapcrc, cfgcrc, idle, replay, content, assisted) "
                                   "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        if(!st) return;
        const inputattempt &first = *run.attempts[0];
        sqlite3_bind_int64(st, 1, info.date);
        bindtext(st, 2, info.map);
        sqlite3_bind_int(st, 3, info.mode);
        bindtext(st, 4, runkindname(info));
        sqlite3_bind_int(st, 5, info.finished);
        if(info.finished) sqlite3_bind_int(st, 6, info.runtime);
        else sqlite3_bind_null(st, 6);
        sqlite3_bind_int(st, 7, info.ticks);
        sqlite3_bind_int(st, 8, info.splices);
        sqlite3_bind_int(st, 9, run.attempts.length());
        sqlite3_bind_int64(st, 10, first.mapcrc);
        if(first.hascfgcrc) sqlite3_bind_int64(st, 11, first.cfgcrc);
        else sqlite3_bind_null(st, 11);
        sqlite3_bind_int(st, 12, replaylongestidle(run));
        bindtext(st, 13, replay);
        if(content) sqlite3_bind_blob(st, 14, content, sbr2::HASHSIZE, SQLITE_TRANSIENT);
        else sqlite3_bind_null(st, 14);
        sqlite3_bind_int(st, 15, tellsassisted(run) ? 1 : 0);
        bool added = sqlite3_step(st) == SQLITE_DONE;
        if(!added) conoutf(CON_ERROR, "stats: %s", sqlite3_errmsg(db));
        sqlite3_finalize(st);
        if(added) { recordsplits(sqlite3_last_insert_rowid(db), info); extendhistory(info, content); }
        changes++;
    }

    // The history chain: head = BLAKE3(head || summary of the attempt) for every attempt
    // recorded here, kept in meta as historyhead (32 bytes) and historycount.
    bool statshistory(uchar *head, int &count, int &attempts)
    {
        memset(head, 0, 32);
        count = attempts = 0;
        sqlite3_stmt *st = prepare("SELECT key, value FROM meta WHERE key IN ('historyhead', 'historycount')");
        if(!st) return false;
        while(sqlite3_step(st) == SQLITE_ROW)
        {
            const char *key = (const char *)sqlite3_column_text(st, 0);
            if(!strcmp(key, "historycount")) count = sqlite3_column_int(st, 1);
            else if(sqlite3_column_bytes(st, 1) == 32) memcpy(head, sqlite3_column_blob(st, 1), 32);
        }
        sqlite3_finalize(st);
        st = prepare("SELECT COUNT(*) FROM attempts");
        if(!st) return false;
        if(sqlite3_step(st) == SQLITE_ROW) attempts = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
        return true;
    }

    // summary = var date, str map, str kind, svar mode, var finished, var runtime ms, var ticks,
    // var splices, content hash (32 bytes, zeros without an SBR2 replay)
    static void historysummary(vector<uchar> &out, uint date, const char *map, const char *kind, int mode, int finished, int runtime, int ticks, int splices, const uchar *content)
    {
        sbr2::byteswriter w(out);
        w.var(date);
        w.str(map);
        w.str(kind);
        w.svar(mode);
        w.var(finished);
        w.var(runtime);
        w.var(ticks);
        w.var(splices);
        uchar zero[32] = { 0 };
        w.bytes(content ? content : zero, 32);
    }

    static void historystep(uchar *head, const vector<uchar> &summary)
    {
        blake3_hasher h;
        blake3_hasher_init(&h);
        blake3_hasher_update(&h, head, 32);
        blake3_hasher_update(&h, summary.getbuf(), summary.length());
        blake3_hasher_finalize(&h, head, 32);
    }

    static void extendhistory(const replayinfo &info, const uchar *content)
    {
        uchar head[32];
        int count, attempts;
        if(!statshistory(head, count, attempts)) return;
        vector<uchar> summary;
        historysummary(summary, info.date, info.map, runkindname(info), info.mode, info.finished, info.finished ? info.runtime : 0, info.ticks, info.splices, content);
        historystep(head, summary);
        sqlite3_stmt *st = prepare("INSERT OR REPLACE INTO meta(key, value) VALUES('historyhead', ?), ('historycount', ?)");
        if(!st) return;
        sqlite3_bind_blob(st, 1, head, 32, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, count + 1);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    // the replay file of the fastest finished run with an existing replay; file is a string
    bool statspbreplay(const char *map, const char *kind, int mode, bool segmented, char *file)
    {
        sqlite3_stmt *st = prepare("SELECT replay FROM attempts WHERE map = ? AND kind = ? AND mode = ? AND finished AND NOT assisted AND (splices > 0) = ? "
                                   "AND replay IS NOT NULL ORDER BY runtime, id LIMIT 1");
        if(!st) return false;
        bindtext(st, 1, map);
        bindtext(st, 2, kind);
        sqlite3_bind_int(st, 3, mode);
        sqlite3_bind_int(st, 4, segmented ? 1 : 0);
        bool found = sqlite3_step(st) == SQLITE_ROW;
        if(found) copystring(file, (const char *)sqlite3_column_text(st, 0), MAXSTRLEN);
        sqlite3_finalize(st);
        return found;
    }

    // the replay files of the current PBs, normal and segmented, of every category that has one
    void statspbreplays(vector<char *> &files)
    {
        sqlite3_stmt *st = prepare(
            "SELECT a.replay FROM attempts a WHERE a.replay IS NOT NULL AND a.id = ("
            "  SELECT b.id FROM attempts b WHERE b.map = a.map AND b.kind = a.kind AND b.mode = a.mode"
            "  AND (b.splices > 0) = (a.splices > 0) AND b.finished AND NOT b.assisted ORDER BY b.runtime, b.id LIMIT 1)");
        if(!st) return;
        while(sqlite3_step(st) == SQLITE_ROW) files.add(newstring((const char *)sqlite3_column_text(st, 0)));
        sqlite3_finalize(st);
    }

    // The PB (fastest finished run of the category, the oldest of equal times): its run time (-1 without one) and splits,
    // and the best time of every segment (previous split, split) over all runs of the category.
    bool statssplitcompare(const char *map, const char *kind, int mode, bool segmented, vector<splittime> &pb, vector<splitsegment> &best, int &pbms)
    {
        pb.setsize(0);
        best.setsize(0);
        pbms = -1;
        sqlite3_stmt *pt = prepare("SELECT MIN(runtime) FROM attempts WHERE map = ? AND kind = ? AND mode = ? AND finished AND NOT assisted AND (splices > 0) = ?");
        if(!pt) return false;
        bindtext(pt, 1, map);
        bindtext(pt, 2, kind);
        sqlite3_bind_int(pt, 3, mode);
        sqlite3_bind_int(pt, 4, segmented ? 1 : 0);
        if(sqlite3_step(pt) == SQLITE_ROW && sqlite3_column_type(pt, 0) != SQLITE_NULL) pbms = sqlite3_column_int(pt, 0);
        sqlite3_finalize(pt);
        sqlite3_stmt *st = prepare("SELECT s.name, s.tick FROM splits s WHERE s.attempt = (SELECT id FROM attempts WHERE map = ? AND kind = ? AND mode = ? "
                                   "AND finished AND NOT assisted AND (splices > 0) = ? ORDER BY runtime, id LIMIT 1) ORDER BY s.idx");
        if(!st) return false;
        bindtext(st, 1, map);
        bindtext(st, 2, kind);
        sqlite3_bind_int(st, 3, mode);
        sqlite3_bind_int(st, 4, segmented ? 1 : 0);
        while(sqlite3_step(st) == SQLITE_ROW)
        {
            splittime &t = pb.add();
            copystring(t.name, (const char *)sqlite3_column_text(st, 0));
            t.tick = sqlite3_column_int(st, 1);
        }
        sqlite3_finalize(st);
        st = prepare("SELECT s.prev, s.name, MIN(s.segment) FROM splits s JOIN attempts a ON a.id = s.attempt "
                     "WHERE a.map = ? AND a.kind = ? AND a.mode = ? AND NOT a.assisted AND (a.splices > 0) = ? GROUP BY s.prev, s.name");
        if(!st) return false;
        bindtext(st, 1, map);
        bindtext(st, 2, kind);
        sqlite3_bind_int(st, 3, mode);
        sqlite3_bind_int(st, 4, segmented ? 1 : 0);
        while(sqlite3_step(st) == SQLITE_ROW)
        {
            splitsegment &g = best.add();
            copystring(g.prev, (const char *)sqlite3_column_text(st, 0));
            copystring(g.name, (const char *)sqlite3_column_text(st, 1));
            g.ticks = sqlite3_column_int(st, 2);
        }
        sqlite3_finalize(st);
        return true;
    }

    // Writes the attempts in the history chain, oldest first: "SBR2-history 1", the count, then
    // per attempt the summary and the head after it, in hex. Anyone can recompute the chain
    // from 32 zero bytes and match it against the heads replays carry.
    static void statsexporthistory(const char *file)
    {
        uchar stored[32], head[32];
        int count = 0, attempts = 0;
        if(!statshistory(stored, count, attempts)) { conoutf(CON_ERROR, "statsexporthistory: no statistics database"); return; }
        sqlite3_stmt *st = prepare("SELECT date, map, kind, mode, finished, runtime, ticks, splices, content FROM "
                                   "(SELECT * FROM attempts ORDER BY id DESC LIMIT ?) ORDER BY id");
        if(!st) return;
        sqlite3_bind_int(st, 1, count);
        stream *f = openutf8file(path(file, true), "w");
        if(!f) { sqlite3_finalize(st); conoutf(CON_ERROR, "statsexporthistory: could not write %s", file); return; }
        f->printf("SBR2-history 1\n%d\n", count);
        memset(head, 0, sizeof(head));
        int n = 0;
        while(sqlite3_step(st) == SQLITE_ROW)
        {
            const uchar *content = sqlite3_column_bytes(st, 8) == 32 ? (const uchar *)sqlite3_column_blob(st, 8) : NULL;
            vector<uchar> summary;
            historysummary(summary, uint(sqlite3_column_int64(st, 0)), (const char *)sqlite3_column_text(st, 1), (const char *)sqlite3_column_text(st, 2),
                           sqlite3_column_int(st, 3), sqlite3_column_int(st, 4), sqlite3_column_int(st, 5), sqlite3_column_int(st, 6), sqlite3_column_int(st, 7), content);
            historystep(head, summary);
            vector<char> hex;
            hex.growbuf(summary.length()*2 + 1);
            sbr2::tohex(summary.getbuf(), summary.length(), hex.getbuf());
            string headhex;
            sbr2::tohex(head, 32, headhex);
            f->printf("%s %s\n", hex.getbuf(), headhex);
            n++;
        }
        sqlite3_finalize(st);
        delete f;
        bool match = n == count && !memcmp(head, stored, 32);
        conoutf(match ? CON_INFO : CON_ERROR, "statsexporthistory: wrote %d attempts to %s%s", n, file, match ? "" : " (they don't reproduce the stored head)");
    }
    COMMAND(statsexporthistory, "s");

    // a replay file was renamed or moved (to = NULL: deleted); its run keeps its row, and a ghost
    // chosen from it follows it or, once deleted, goes back to the PB
    void statsreplaymoved(const char *from, const char *to)
    {
        sqlite3_stmt *st = prepare("UPDATE attempts SET replay = ? WHERE replay = ?");
        if(!st) return;
        bindtext(st, 1, to);
        bindtext(st, 2, from);
        sqlite3_step(st);
        sqlite3_finalize(st);
        st = prepare(to ? "UPDATE ghosts SET file = ? WHERE file = ?" : "DELETE FROM ghosts WHERE file = ?");
        if(!st) return;
        if(to) { bindtext(st, 1, to); bindtext(st, 2, from); }
        else bindtext(st, 1, from);
        sqlite3_step(st);
        sqlite3_finalize(st);
        changes++;
    }

    // the splits recorded for the run saved as this replay; false if it has no run here
    bool statssplitsofreplay(const char *file, vector<splittime> &out)
    {
        out.setsize(0);
        sqlite3_stmt *st = prepare("SELECT s.name, s.tick FROM splits s JOIN attempts a ON a.id = s.attempt WHERE a.replay = ? ORDER BY s.idx");
        if(!st) return false;
        bindtext(st, 1, file);
        while(sqlite3_step(st) == SQLITE_ROW)
        {
            splittime &t = out.add();
            copystring(t.name, (const char *)sqlite3_column_text(st, 0));
            t.tick = sqlite3_column_int(st, 1);
        }
        sqlite3_finalize(st);
        return out.length() > 0;
    }

    bool statsghostchoice(const char *map, const char *kind, int mode, char *source, char *file, char *label)
    {
        sqlite3_stmt *st = prepare("SELECT source, file, label FROM ghosts WHERE map = ? AND kind = ? AND mode = ?");
        if(!st) return false;
        bindtext(st, 1, map);
        bindtext(st, 2, kind);
        sqlite3_bind_int(st, 3, mode);
        bool found = sqlite3_step(st) == SQLITE_ROW;
        if(found)
        {
            const char *s = (const char *)sqlite3_column_text(st, 0), *f = (const char *)sqlite3_column_text(st, 1), *l = (const char *)sqlite3_column_text(st, 2);
            copystring(source, s ? s : "pb", MAXSTRLEN);
            copystring(file, f ? f : "", MAXSTRLEN);
            copystring(label, l ? l : "", MAXSTRLEN);
        }
        sqlite3_finalize(st);
        return found;
    }

    void statssetghost(const char *map, const char *kind, int mode, const char *source, const char *file, const char *label)
    {
        sqlite3_stmt *st = prepare("INSERT OR REPLACE INTO ghosts(map, kind, mode, source, file, label) VALUES(?, ?, ?, ?, ?, ?)");
        if(!st) return;
        bindtext(st, 1, map);
        bindtext(st, 2, kind);
        sqlite3_bind_int(st, 3, mode);
        bindtext(st, 4, source);
        bindtext(st, 5, file && file[0] ? file : NULL);
        bindtext(st, 6, label && label[0] ? label : NULL);
        if(sqlite3_step(st) != SQLITE_DONE) conoutf(CON_ERROR, "stats: %s", sqlite3_errmsg(db));
        sqlite3_finalize(st);
        changes++;
    }

    static const char *formatms(int ms)
    {
        static string buf;
        formatstring(buf, "%d:%02d.%03d", ms/60000, (ms/1000)%60, ms%1000);
        return buf;
    }

    static const char *formatduration(int64_t ms)
    {
        static string buf;
        int s = int(ms/1000);
        if(s >= 3600) formatstring(buf, "%dh %02dm", s/3600, (s/60)%60);
        else formatstring(buf, "%dm %02ds", s/60, s%60);
        return buf;
    }

    // flagruns are told apart by mode: effic ctf, insta ctf, and plain ctf without weapons
    static const char *kinddisplay(const char *kind, int mode)
    {
        if(!strcmp(kind, "flagrun"))
            return m_check(mode, M_EFFICIENCY) ? "effic flagrun" : m_check(mode, M_INSTA) ? "insta flagrun" : "weaponless flagrun";
        if(!strcmp(kind, "sp")) return m_check(mode, M_DMSP) ? "dmsp" : "sp";
        return kind;
    }

    // per map, kind and mode: normal runs and segmented runs counted apart
    struct mapstats
    {
        string map, kind;
        int mode, runs, finished, pb, segruns, segpb, avg10;
        int64_t ticks, last, pbdate;
    };
    static vector<mapstats> maplist;

    MOD(VARP, statskind, 0, 0, 4); // numbered like replayfilterkind
    MOD(VARP, statssort, 0, 0, 3); // 0 recently played, 1 map, 2 runs, 3 time played
    MOD(VARP, statspbonly, 0, 0, 1);
    MOD(VARP, statsflagmode, 0, 0, 3); // numbered like replayfilterflagmode
    extern int flagrunmodeof(int mode);

    static int64_t queryint64(const char *sql, const char *map, const char *kind, int mode, int64_t def)
    {
        sqlite3_stmt *st = prepare(sql);
        if(!st) return def;
        bindtext(st, 1, map);
        bindtext(st, 2, kind);
        sqlite3_bind_int(st, 3, mode);
        int64_t val = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL ? sqlite3_column_int64(st, 0) : def;
        sqlite3_finalize(st);
        return val;
    }
    static int queryint(const char *sql, const char *map, const char *kind, int mode, int def) { return int(queryint64(sql, map, kind, mode, def)); }

    static int statskindof(const mapstats &m)
    {
        if(!strcmp(m.kind, "race")) return 4;
        if(!strcmp(m.kind, "flagrun")) return 3;
        if(!strcmp(m.kind, "sp")) return m_check(m.mode, M_DMSP) ? 2 : 1;
        return 0;
    }

    static bool statsbymap(const mapstats &a, const mapstats &b)
    {
        int c = strcasecmp(a.map, b.map);
        return c ? c < 0 : a.last > b.last;
    }
    static bool statsbyruns(const mapstats &a, const mapstats &b) { return a.runs + a.segruns != b.runs + b.segruns ? a.runs + a.segruns > b.runs + b.segruns : a.last > b.last; }
    static bool statsbyplayed(const mapstats &a, const mapstats &b) { return a.ticks != b.ticks ? a.ticks > b.ticks : a.last > b.last; }

    // statsscan [map filter]: caches the per-map summary, most recently played first unless
    // statssort says otherwise; statskind and statspbonly narrow it down
    ICOMMAND(statsscan, "s", (char *filter),
    {
        maplist.setsize(0);
        sqlite3_stmt *st = prepare(
            "SELECT map, kind, mode, SUM(splices = 0), SUM(finished AND splices = 0),"
            "  MIN(CASE WHEN finished AND NOT assisted AND splices = 0 THEN runtime END),"
            "  SUM(splices > 0), MIN(CASE WHEN finished AND NOT assisted AND splices > 0 THEN runtime END),"
            "  SUM(ticks), MAX(date) "
            "FROM attempts WHERE instr(lower(map), lower(?)) GROUP BY map, kind, mode ORDER BY MAX(date) DESC");
        if(!st) { intret(0); return; }
        bindtext(st, 1, filter);
        while(sqlite3_step(st) == SQLITE_ROW)
        {
            mapstats &m = maplist.add();
            copystring(m.map, (const char *)sqlite3_column_text(st, 0));
            copystring(m.kind, (const char *)sqlite3_column_text(st, 1));
            m.mode = sqlite3_column_int(st, 2);
            m.runs = sqlite3_column_int(st, 3);
            m.finished = sqlite3_column_int(st, 4);
            m.pb = sqlite3_column_type(st, 5) != SQLITE_NULL ? sqlite3_column_int(st, 5) : -1;
            m.segruns = sqlite3_column_int(st, 6);
            m.segpb = sqlite3_column_type(st, 7) != SQLITE_NULL ? sqlite3_column_int(st, 7) : -1;
            m.ticks = sqlite3_column_int64(st, 8);
            m.last = sqlite3_column_int64(st, 9);
            if((statskind && statskindof(m) != statskind) || (statspbonly && m.pb < 0 && m.segpb < 0) ||
               (statskind == 3 && statsflagmode && flagrunmodeof(m.mode) != statsflagmode))
                maplist.pop();
        }
        sqlite3_finalize(st);
        loopv(maplist)
        {
            mapstats &m = maplist[i];
            m.avg10 = queryint("SELECT AVG(runtime) FROM (SELECT runtime FROM attempts WHERE map = ? AND kind = ? AND mode = ? AND finished AND NOT assisted AND splices = 0 "
                               "ORDER BY date DESC, id DESC LIMIT 10)", m.map, m.kind, m.mode, -1);
            m.pbdate = queryint64("SELECT date FROM attempts WHERE map = ? AND kind = ? AND mode = ? AND finished AND NOT assisted AND splices = 0 "
                                  "ORDER BY runtime, date, id LIMIT 1", m.map, m.kind, m.mode, -1);
        }
        if(statssort == 1) maplist.sort(statsbymap);
        else if(statssort == 2) maplist.sort(statsbyruns);
        else if(statssort == 3) maplist.sort(statsbyplayed);
        intret(maplist.length());
    });

    ICOMMAND(statsmapfield, "is", (int *i, char *field),
    {
        if(!maplist.inrange(*i)) { result(""); return; }
        const mapstats &m = maplist[*i];
        if(!strcmp(field, "map")) result(m.map);
        else if(!strcmp(field, "kind")) result(m.kind);
        else if(!strcmp(field, "mode")) intret(m.mode);
        else if(!strcmp(field, "kindname")) result(kinddisplay(m.kind, m.mode));
        else if(!strcmp(field, "runs")) intret(m.runs);
        else if(!strcmp(field, "finished")) intret(m.finished);
        else if(!strcmp(field, "pb")) result(m.pb >= 0 ? formatms(m.pb) : "-");
        else if(!strcmp(field, "pbdate"))
        {
            if(m.pbdate < 0) { result(""); return; }
            time_t t = time_t(m.pbdate);
            time_t now = time(NULL);
            tm when = *localtime(&t);
            int year = localtime(&now)->tm_year;
            string buf;
            strftime(buf, sizeof(buf), when.tm_year == year ? "%d %b" : "%d %b %y", &when);
            result(buf);
        }
        else if(!strcmp(field, "avg")) result(m.avg10 >= 0 ? formatms(m.avg10) : "-");
        else if(!strcmp(field, "segruns")) intret(m.segruns);
        else if(!strcmp(field, "segpb")) result(m.segpb >= 0 ? formatms(m.segpb) : "-");
        else if(!strcmp(field, "played")) result(formatduration(m.ticks*SIMTICK));
        else result("");
    });

    struct attemptstats
    {
        int64_t date;
        int finished, runtime, ticks, splices, idle;
        string replay;
    };
    static vector<attemptstats> attemptlist;

    // statsattempts map kind mode: caches the newest 100 runs of a map
    ICOMMAND(statsattempts, "ssi", (char *map, char *kind, int *mode),
    {
        attemptlist.setsize(0);
        sqlite3_stmt *st = prepare("SELECT date, finished, runtime, ticks, splices, idle, replay FROM attempts "
                                   "WHERE map = ? AND kind = ? AND mode = ? ORDER BY date DESC, id DESC LIMIT 100");
        if(!st) { intret(0); return; }
        bindtext(st, 1, map);
        bindtext(st, 2, kind);
        sqlite3_bind_int(st, 3, *mode);
        while(sqlite3_step(st) == SQLITE_ROW)
        {
            attemptstats &a = attemptlist.add();
            a.date = sqlite3_column_int64(st, 0);
            a.finished = sqlite3_column_int(st, 1);
            a.runtime = sqlite3_column_int(st, 2);
            a.ticks = sqlite3_column_int(st, 3);
            a.splices = sqlite3_column_int(st, 4);
            a.idle = sqlite3_column_int(st, 5);
            const char *replay = (const char *)sqlite3_column_text(st, 6);
            copystring(a.replay, replay ? replay : "");
        }
        sqlite3_finalize(st);
        intret(attemptlist.length());
    });

    ICOMMAND(statsattemptfield, "is", (int *i, char *field),
    {
        if(!attemptlist.inrange(*i)) { result(""); return; }
        const attemptstats &a = attemptlist[*i];
        if(!strcmp(field, "date") || !strcmp(field, "time"))
        {
            time_t t = time_t(a.date);
            tm *lt = localtime(&t);
            string buf;
            if(field[0] == 'd') formatstring(buf, "%04d-%02d-%02d", lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday);
            else formatstring(buf, "%02d:%02d", lt->tm_hour, lt->tm_min);
            result(buf);
        }
        else if(!strcmp(field, "finished")) intret(a.finished);
        else if(!strcmp(field, "runtime")) result(a.finished ? formatms(a.runtime) : "DNF");
        else if(!strcmp(field, "length")) result(formatduration(int64_t(a.ticks)*SIMTICK));
        else if(!strcmp(field, "splices")) intret(a.splices);
        else if(!strcmp(field, "idle")) result(formatduration(int64_t(a.idle)*SIMTICK));
        else if(!strcmp(field, "replay")) result(a.replay);
        else result("");
    });

    ICOMMAND(statsplay, "i", (int *i),
    {
        if(!attemptlist.inrange(*i) || !attemptlist[*i].replay[0] || multiplayer(false)) return;
        inputrun run;
        replayinfo info;
        if(!readreplay(attemptlist[*i].replay, &run, info)) { conoutf(CON_ERROR, "could not read %s", attemptlist[*i].replay); return; }
        playinputrun(run, attemptlist[*i].replay);
    });
#endif
}
