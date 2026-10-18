// Run statistics: one row per started run in speedbraten.db (SQLite) in the home dir, written
// when the run ends. Runs with state loads (splices > 0) are a separate category from normal
// runs. Nothing here reads or changes simulation state beyond what the finished run reports.

#include "game.h"
#include "inputs.h"
#ifndef HEADLESS
#include "sqlite3.h"
#endif

namespace game
{
#ifdef HEADLESS
    void statsrecord(const inputrun &run, const replayinfo &info, const char *replay) {}
    void statsreplaymoved(const char *from, const char *to) {}
    bool statspbreplay(const char *map, const char *kind, int mode, bool segmented, char *file) { return false; }
    int statsversion() { return 0; }
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
        "CREATE INDEX IF NOT EXISTS attempts_map ON attempts(map, kind, finished);";

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

    static int longestidle(const inputrun &run)
    {
        int idle = 0;
        loopvk(run.attempts)
        {
            const inputattempt &a = *run.attempts[k];
            int last = a.starttick;
            loopv(a.events)
            {
                const inputevent &e = a.events[i];
                if(e.type == INPUT_SETTING) continue;
                idle = max(idle, e.tick - last);
                last = e.tick;
            }
            idle = max(idle, a.ticks - last);
        }
        return idle;
    }

    static const char *runkindname(const replayinfo &info)
    {
        return info.race ? "race" : info.flagrun ? "flagrun" : "sp";
    }

    void statsrecord(const inputrun &run, const replayinfo &info, const char *replay)
    {
        sqlite3_stmt *st = prepare("INSERT INTO attempts(date, map, mode, kind, finished, runtime, ticks, splices, maps, mapcrc, cfgcrc, idle, replay) "
                                   "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
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
        sqlite3_bind_int(st, 12, longestidle(run));
        bindtext(st, 13, replay);
        if(sqlite3_step(st) != SQLITE_DONE) conoutf(CON_ERROR, "stats: %s", sqlite3_errmsg(db));
        sqlite3_finalize(st);
        changes++;
    }

    // the replay file of the fastest finished run with an existing replay; file is a string
    bool statspbreplay(const char *map, const char *kind, int mode, bool segmented, char *file)
    {
        sqlite3_stmt *st = prepare("SELECT replay FROM attempts WHERE map = ? AND kind = ? AND mode = ? AND finished AND (splices > 0) = ? "
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

    // a replay file was renamed or moved (to = NULL: deleted); its run keeps its row
    void statsreplaymoved(const char *from, const char *to)
    {
        sqlite3_stmt *st = prepare("UPDATE attempts SET replay = ? WHERE replay = ?");
        if(!st) return;
        bindtext(st, 1, to);
        bindtext(st, 2, from);
        sqlite3_step(st);
        sqlite3_finalize(st);
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
        int64_t ticks, last;
    };
    static vector<mapstats> maplist;

    static int queryint(const char *sql, const char *map, const char *kind, int mode, int def)
    {
        sqlite3_stmt *st = prepare(sql);
        if(!st) return def;
        bindtext(st, 1, map);
        bindtext(st, 2, kind);
        sqlite3_bind_int(st, 3, mode);
        int val = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL ? sqlite3_column_int(st, 0) : def;
        sqlite3_finalize(st);
        return val;
    }

    // statsscan [map filter]: caches the per-map summary, most recently played first
    ICOMMAND(statsscan, "s", (char *filter),
    {
        maplist.setsize(0);
        sqlite3_stmt *st = prepare(
            "SELECT map, kind, mode, SUM(splices = 0), SUM(finished AND splices = 0),"
            "  MIN(CASE WHEN finished AND splices = 0 THEN runtime END),"
            "  SUM(splices > 0), MIN(CASE WHEN finished AND splices > 0 THEN runtime END),"
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
        }
        sqlite3_finalize(st);
        loopv(maplist)
            maplist[i].avg10 = queryint("SELECT AVG(runtime) FROM (SELECT runtime FROM attempts WHERE map = ? AND kind = ? AND mode = ? AND finished AND splices = 0 "
                                        "ORDER BY date DESC, id DESC LIMIT 10)", maplist[i].map, maplist[i].kind, maplist[i].mode, -1);
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
        playinputrun(run);
    });
#endif
}
