// sauer_witness: receives replays while they are played and signs what it received. Clients
// connect over the Noise channel (shared/noise.h, messages in shared/witnessproto.h) and stream
// each run's replay file as it is written. For every run the witness
//   - parses the bytes as they arrive (sbr2::streamreader) and keeps them in a file,
//   - checks that every map attempt uses a seed it handed out,
//   - checks every TICK commitment against the state hash stored for that tick,
//   - bounds how far the run's ticks fall behind or run ahead of its own clock,
//   - signs each segment's chain head as it closes and the content hash once END arrives.
// Every finished run is then re-simulated by sauer_resim (one at a time, in its own process)
// with the leaderboard rules and, for a flagrun or race life, a check of the life's saved
// start; the witness signs the verdict and sends it to the player if still connected.
// Sessions, runs, seeds, signatures and verdicts are recorded in witness.db (SQLite).
//
//   sauer_witness -d<data dir> -s<signing seed file> [-c<channel key file>] [-p<port>]
//                 [-e<sauer_resim> -r<game data dir> [-o<overrides dir>] [-R]]
//
// Without -e and -r runs are signed but get no verdict. -R judges every finished run again.
//
// The channel key file holds the X25519 private key in hex; it is created if missing. The
// signing seed comes from server/cmd/witnesskeys.

#include "cube.h"
#include "noise.h"
#include "witnessproto.h"
#include "replayio.h"
#include "sqlite3.h"

#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>

void conoutfv(int type, const char *fmt, va_list args)
{
    vfprintf(stdout, fmt, args);
    fputc('\n', stdout);
    fflush(stdout);
}

void fatal(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

namespace witness
{
    enum { MAXCONNECTIONS = 256, MAXPERADDRESS = 4, HANDSHAKETIMEOUT = 10000 };
    // a run's ticks may fall behind the witness's clock by up to LAG ms (network delay, frame
    // hitches) and run ahead of it by up to LEAD ms
    enum { LAG = 2000, LEAD = 500 };
    // a run whose player gives no input for this many ticks (30 s) is no longer watched
    enum { AFKTICKS = 6000 };
    // the input event type of a setting change (INPUT_SETTING in fpsgame/game.h), which isn't the player's doing
    enum { EVENT_SETTING = 6 };

    static uchar channelkey[noise::KEYLEN], signingseed[32];
    static string datadir = "", resimexe = "", resimdata = "", resimoverrides = "";
    enum { RESIMCPU = 600, RESIMWALL = 900 }; // seconds
    static sqlite3 *db = NULL;
    static unsigned long long signcounter = 0;

    static unsigned long long nowus()
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return (unsigned long long)tv.tv_sec * 1000000 + tv.tv_usec;
    }

    static void tohexstr(const uchar *p, int n, char *out) { sbr2::tohex(p, n, out); out[2*n] = '\0'; }

    static bool exec(const char *sql)
    {
        char *err = NULL;
        if(sqlite3_exec(db, sql, NULL, NULL, &err) == SQLITE_OK) return true;
        conoutf("witness.db: %s", err ? err : "error");
        sqlite3_free(err);
        return false;
    }

    // runs the statement with its parameters bound in order (integers as long long, text as
    // char *); returns the new row id for inserts
    static long long run(const char *sql, const char *types, ...)
    {
        sqlite3_stmt *st = NULL;
        if(sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) { conoutf("witness.db: %s", sqlite3_errmsg(db)); return -1; }
        va_list args;
        va_start(args, types);
        for(int i = 0; types[i]; i++) switch(types[i])
        {
            case 'i': sqlite3_bind_int64(st, i+1, va_arg(args, long long)); break;
            case 's': sqlite3_bind_text(st, i+1, va_arg(args, const char *), -1, SQLITE_TRANSIENT); break;
            case 'b': { const uchar *p = va_arg(args, const uchar *); int n = va_arg(args, int); sqlite3_bind_blob(st, i+1, p, n, SQLITE_TRANSIENT); break; }
        }
        va_end(args);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if(rc != SQLITE_DONE) { conoutf("witness.db: %s", sqlite3_errmsg(db)); return -1; }
        return sqlite3_last_insert_rowid(db);
    }

    // for databases made before the column existed
    static bool addcolumn(const char *table, const char *column)
    {
        defformatstring(sql, "ALTER TABLE %s ADD COLUMN %s", table, column);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
        return true;
    }

    static bool opendb()
    {
        defformatstring(file, "%s/witness.db", datadir);
        if(sqlite3_open(file, &db) != SQLITE_OK) return false;
        return exec("PRAGMA journal_mode=WAL;"
            "CREATE TABLE IF NOT EXISTS sessions(id INTEGER PRIMARY KEY, session TEXT, installkey TEXT, address TEXT, build TEXT, started INTEGER, ended INTEGER);"
            "CREATE TABLE IF NOT EXISTS seeds(session INTEGER, seed INTEGER, issued INTEGER, used INTEGER);"
            "CREATE TABLE IF NOT EXISTS runs(id INTEGER PRIMARY KEY, session INTEGER, runid INTEGER, installkey TEXT, kind INTEGER, starttick INTEGER, started INTEGER, "
            "file TEXT, bytes INTEGER DEFAULT 0, segments INTEGER DEFAULT 0, content TEXT, finished INTEGER DEFAULT 0, violations INTEGER DEFAULT 0, maxlagms INTEGER DEFAULT 0, "
            "seedsok INTEGER DEFAULT 1, stop TEXT, refused TEXT, ended INTEGER);"
            "CREATE TABLE IF NOT EXISTS signatures(run INTEGER, scope INTEGER, segment INTEGER, recvus INTEGER, payload BLOB);") &&
            addcolumn("runs", "verdict TEXT") && addcolumn("runs", "reason TEXT") && addcolumn("runs", "resim TEXT") && addcolumn("sessions", "nonce TEXT") && addcolumn("runs", "judged INTEGER") && addcolumn("runs", "follows INTEGER");
    }

    static vector<long long> resimqueue;

    struct witnessrun
    {
        long long row;
        int runid, kind, starttick, maxlagms, violations, inpslo, segticks;
        unsigned long long t0;
        bool seedsok, done;
        string refused;
        sbr2::streamreader reader;
        int seenchunks, bytes;
        stream *file;
        vector<int> committedticks;
        vector<uint> committedhashes;
        hashtable<int, uint> stored; // tick -> combined state hash, from the HASH chunks
        vector<vector<uchar> > signatures;
        int livepid; // the re-simulation playing the run while it streams, 0 without one
        int lastinput; // tick of the player's last input event, or of the attempt's start
        long long follows; // the row of the run this one continues (the next run of a life), -1 if none
        bool ended;        // its END was taken

        witnessrun() : row(-1), runid(0), kind(0), starttick(0), maxlagms(0), violations(0), inpslo(0), segticks(0), t0(0), seedsok(true), done(false), seenchunks(0), bytes(0), file(NULL), livepid(0), lastinput(0), follows(-1), ended(false) { refused[0] = '\0'; }
        ~witnessrun() { DELETEP(file); }
    };

    struct client
    {
        ENetSocket sock;
        ENetAddress address;
        string addressname, build;
        int connected;
        noise::handshake *hs;
        noise::cipherstate send, recv;
        bool open, closing;
        vector<uchar> in, out;
        uchar session[16], nonce[16], installid[sbr2::signature::INSTALLKEYID];
        string installhex;
        long long sessionrow;
        vector<uint> seeds;     // handed out, in order
        vector<bool> seedsused;
        vector<witnessrun *> runs;

        client() : sock(ENET_SOCKET_NULL), connected(0), hs(NULL), open(false), closing(false), sessionrow(-1) { addressname[0] = build[0] = installhex[0] = '\0'; }
        ~client() { DELETEP(hs); runs.deletecontents(); if(sock != ENET_SOCKET_NULL) enet_socket_destroy(sock); }
    };
    static vector<client *> clients;

    static void frame(client &c, const vector<uchar> &msg)
    {
        c.out.add(uchar(msg.length() >> 8));
        c.out.add(uchar(msg.length()));
        c.out.put(msg.getbuf(), msg.length());
    }

    static void sendmsg(client &c, int type, const vector<uchar> &payload)
    {
        if(!c.open || c.closing) return;
        vector<uchar> plain, msg;
        plain.add(uchar(type));
        plain.put(payload.getbuf(), payload.length());
        if(!c.send.encrypt(NULL, 0, plain.getbuf(), plain.length(), msg)) { c.closing = true; return; }
        frame(c, msg);
    }

    static uint newseed(client &c)
    {
        uint seed = 0;
        while(!seed) noise::randombytes((uchar *)&seed, sizeof(seed));
        c.seeds.add(seed);
        c.seedsused.add(false);
        run("INSERT INTO seeds(session, seed, issued, used) VALUES(?, ?, ?, 0)", "iii", c.sessionrow, (long long)seed, (long long)(nowus()/1000));
        return seed;
    }

    static void sendseed(client &c)
    {
        vector<uchar> p;
        sbr2::byteswriter(p).u32(newseed(c));
        sendmsg(c, WM_SEED, p);
    }

    static void livestart(client &c, witnessrun &r);
    static void livecontinue(client &c, witnessrun &r);
    static void releaseidle(long long session);
    static void livemore(witnessrun &r);
    static void liveend(witnessrun &r);
    static void liveabandon(witnessrun &r);

    // the run is no longer watched; its file stays as far as it got
    static void closerun(witnessrun &r, const char *why)
    {
        if(r.done) return;
        r.done = true;
        DELETEP(r.file);
        liveabandon(r);
        run("UPDATE runs SET stop = ?, violations = ?, maxlagms = ?, ended = ? WHERE id = ?", "siiii", why, (long long)r.violations, (long long)r.maxlagms, (long long)(nowus()/1000), r.row);
    }

    static void stoprun(client &c, witnessrun &r, const char *why)
    {
        if(r.done) return;
        closerun(r, why);
        vector<uchar> p;
        sbr2::byteswriter w(p);
        w.var(r.runid);
        w.str(why);
        sendmsg(c, WM_RUNSTOP, p);
        conoutf("%s: run %d stopped by the witness: %s", c.addressname, r.runid, why);
    }

    static void refuse(client &c, witnessrun &r, const char *why)
    {
        if(r.refused[0] || r.done) return;
        copystring(r.refused, why);
        r.done = true;
        liveabandon(r);
        run("UPDATE runs SET refused = ?, ended = ? WHERE id = ?", "sii", why, (long long)(nowus()/1000), r.row);
        vector<uchar> p;
        sbr2::byteswriter w(p);
        w.var(r.runid);
        w.str(why);
        sendmsg(c, WM_RUNERROR, p);
        conoutf("%s: run %d refused: %s", c.addressname, r.runid, why);
    }

    static void sign(client &c, witnessrun &r, int scope, int segment, const uchar *hash, const vector<uchar> &extras)
    {
        sbr2::signature s;
        s.scope = scope;
        s.segment = segment;
        memcpy(s.session, c.session, sizeof(s.session));
        memcpy(s.nonce, c.nonce, sizeof(s.nonce));
        memcpy(s.installkey, c.installid, sizeof(s.installkey));
        s.counter = ++signcounter;
        s.recvus = nowus();
        memcpy(s.hash, hash, sbr2::HASHSIZE);
        s.extras = extras;
        s.sign(signingseed);
        vector<uchar> payload;
        s.write(payload);
        run("INSERT INTO signatures(run, scope, segment, recvus, payload) VALUES(?, ?, ?, ?, ?)", "iiiib", r.row, (long long)scope, (long long)segment, (long long)s.recvus, payload.getbuf(), payload.length());
        r.signatures.add(payload);
        vector<uchar> msg;
        sbr2::byteswriter w(msg);
        w.var(r.runid);
        w.bytes(payload.getbuf(), payload.length());
        sendmsg(c, WM_SIGN, msg);
    }

    static bool checkcommitments(client &c, witnessrun &r, int below)
    {
        for(int i = 0; i < r.committedticks.length(); i++)
        {
            int tick = r.committedticks[i];
            if(tick >= below) continue;
            uint *want = r.stored.access(tick);
            if(!want || *want != r.committedhashes[i])
            {
                defformatstring(why, "the state hash committed for tick %d (%08x) does not match the replay (%s%08x)", tick, r.committedhashes[i], want ? "" : "none, ", want ? *want : 0);
                refuse(c, r, why);
                return false;
            }
            r.committedticks.remove(i);
            r.committedhashes.remove(i--);
        }
        return true;
    }

    // a newly parsed chunk of a run's file
    static void handlechunk(client &c, witnessrun &r, const sbr2::chunk &ch)
    {
        sbr2::bytesreader b(ch.data.getbuf(), ch.data.length());
        if(!memcmp(ch.type, "ATTM", 4))
        {
            string map;
            b.var();
            b.str(map, sizeof(map));
            b.u32(); b.u8(); b.u32();
            uint seed = b.u32();
            string team;
            b.str(team, sizeof(team));
            b.svar(); b.var(); b.var();
            r.lastinput = int(b.var());
            int found = c.seeds.find(seed);
            if(!b.ok || found < 0) { r.seedsok = false; run("UPDATE runs SET seedsok = 0 WHERE id = ?", "i", r.row); }
            else if(!c.seedsused[found]) { c.seedsused[found] = true; run("UPDATE seeds SET used = ? WHERE session = ? AND seed = ?", "iii", (long long)(nowus()/1000), c.sessionrow, (long long)seed); sendseed(c); }
            r.segticks = 0;
            if(r.livepid) livemore(r);
            else livestart(c, r);
        }
        else if(!memcmp(ch.type, "INPS", 4))
        {
            r.inpslo = int(b.var());
            r.segticks = int(b.var());
            // a new segment shows the run was still played at its first tick
            if(r.inpslo - r.lastinput > AFKTICKS) { stoprun(c, r, "no input for 30 seconds"); return; }
            int n = int(b.var()), tick = r.inpslo;
            vector<int> ticks;
            loopi(n) { tick += int(b.var()); ticks.add(tick); }
            loopi(n) if(b.u8() != EVENT_SETTING && b.ok) r.lastinput = max(r.lastinput, ticks[i]);
        }
        else if(!memcmp(ch.type, "HASH", 4))
        {
            int n = int(b.var()), tick = r.inpslo;
            if(!b.ok || n < 0 || n > b.left()) { refuse(c, r, "unreadable HASH chunk"); return; }
            vector<int> ticks;
            loopi(n) { tick += int(b.var()); ticks.add(tick); }
            loopi(n) r.stored[ticks[i]] = b.u32();
        }
        else if(!memcmp(ch.type, "SEGE", 4))
        {
            if(!checkcommitments(c, r, r.inpslo + r.segticks)) return;
            int index = r.reader.heads.length() / sbr2::HASHSIZE - 1;
            vector<uchar> none;
            sign(c, r, sbr2::signature::SEGMENT, index, &r.reader.heads[index * sbr2::HASHSIZE], none);
            run("UPDATE runs SET segments = ? WHERE id = ?", "ii", (long long)(index + 1), r.row);
            livemore(r);
        }
        else if(!memcmp(ch.type, "END ", 4))
        {
            if(!checkcommitments(c, r, INT_MAX)) return;
            vector<uchar> extras;
            sbr2::kvwriter kv;
            kv.boolean("realtime", r.violations == 0);
            kv.var("violations", r.violations);
            kv.var("maxlagms", r.maxlagms);
            kv.boolean("seedsok", r.seedsok);
            kv.build(extras);
            sign(c, r, sbr2::signature::FINAL, 0, r.reader.content, extras);
            r.done = r.ended = true;
            string hex;
            tohexstr(r.reader.content, sbr2::HASHSIZE, hex);
            run("UPDATE runs SET finished = 1, content = ?, violations = ?, maxlagms = ?, ended = ? WHERE id = ?", "siiii", hex, (long long)r.violations, (long long)r.maxlagms, (long long)(nowus()/1000), r.row);
            if(r.file)
            {
                loopv(r.signatures) { vector<uchar> chunk; sbr2::appendchunk(chunk, "sIGN", r.signatures[i]); r.file->write(chunk.getbuf(), chunk.length()); }
                DELETEP(r.file);
            }
            conoutf("%s: run %d signed: %d segments, %d bytes, %s, %d violations, max lag %d ms", c.addressname, r.runid, r.reader.segment, r.bytes, r.seedsok ? "witness seeds" : "unknown seed", r.violations, r.maxlagms);
            if(r.livepid) liveend(r);
            else if(resimexe[0]) resimqueue.add(r.row);
        }
    }

    static witnessrun *findrun(client &c, int runid)
    {
        loopv(c.runs) if(c.runs[i]->runid == runid) return c.runs[i];
        return NULL;
    }

    static void handlemessage(client &c, int type, const uchar *p, int len)
    {
        sbr2::bytesreader b(p, len);
        int runid = int(b.var());
        if(!b.ok) { c.closing = true; return; }
        witnessrun *r = findrun(c, runid);
        switch(type)
        {
            case WM_RUNSTART:
            {
                if(r) { c.closing = true; return; }
                loopv(c.runs) if(!c.runs[i]->done) closerun(*c.runs[i], "a new run started");
                int kind = int(b.var()), starttick = int(b.var()), continues = kind == WITNESSRUN_CHAIN ? int(b.var()) : -1;
                witnessrun *before = continues >= 0 ? findrun(c, continues) : NULL;
                loopv(c.runs) if(c.runs[i]->runid > continues) before = NULL;
                if(continues < 0) releaseidle(c.sessionrow);
                r = c.runs.add(new witnessrun);
                r->runid = runid;
                r->kind = kind;
                r->starttick = starttick;
                r->t0 = nowus();
                defformatstring(dir, "%s/runs/%s", datadir, c.installhex);
                mkdir(dir, 0755);
                defformatstring(file, "%s/%lld-%d.sbr", dir, c.sessionrow, runid);
                r->file = openrawfile(file, "wb");
                r->row = run("INSERT INTO runs(session, runid, installkey, kind, starttick, started, file) VALUES(?, ?, ?, ?, ?, ?, ?)", "iisiiis",
                    c.sessionrow, (long long)runid, c.installhex, (long long)r->kind, (long long)r->starttick, (long long)(r->t0/1000), file);
                if(continues >= 0)
                {
                    if(!before || !before->ended) { refuse(c, *r, "it continues a run the witness did not see end"); break; }
                    r->follows = before->row;
                    run("UPDATE runs SET follows = ? WHERE id = ?", "ii", before->row, r->row);
                    livecontinue(c, *r);
                }
                break;
            }
            case WM_DATA:
            {
                if(!r || r->done) return;
                int n = b.left();
                r->bytes += n;
                if(r->file) r->file->write(b.p, n);
                if(!r->reader.feed(b.p, n, sbr2::KNOWNCHUNKS)) { refuse(c, *r, r->reader.error); return; }
                while(r->seenchunks < r->reader.chunks.length() && !r->done) handlechunk(c, *r, *r->reader.chunks[r->seenchunks++]);
                run("UPDATE runs SET bytes = ? WHERE id = ?", "ii", (long long)r->bytes, r->row);
                break;
            }
            case WM_TICK:
            {
                if(!r || r->done) return;
                int tick = int(b.var());
                uint hash = b.u32();
                if(!b.ok) { c.closing = true; return; }
                long long expected = (long long)r->t0 + (long long)(tick - r->starttick) * 5000, lag = ((long long)nowus() - expected) / 1000;
                if(lag > LAG || lag < -LEAD) r->violations++;
                r->maxlagms = max(r->maxlagms, int(lag));
                uint *want = r->stored.access(tick);
                if(want && *want != hash)
                {
                    defformatstring(why, "the state hash committed for tick %d (%08x) does not match the replay (%08x)", tick, hash, *want);
                    refuse(c, *r, why);
                }
                else if(!want) { r->committedticks.add(tick); r->committedhashes.add(hash); }
                break;
            }
            case WM_STOP:
            {
                if(!r || r->done) return;
                string reason;
                b.str(reason, sizeof(reason));
                closerun(*r, reason);
                conoutf("%s: run %d stopped: %s", c.addressname, runid, reason);
                break;
            }
            default: c.closing = true; return;
        }
    }

    static void handshakemessage(client &c, const uchar *msg, int len)
    {
        vector<uchar> payload;
        if(!c.hs->readmessage(msg, len, payload)) { conoutf("%s: handshake refused", c.addressname); c.closing = true; return; }
        if(c.hs->step == 1)
        {
            noise::randombytes(c.session, sizeof(c.session));
            noise::randombytes(c.nonce, sizeof(c.nonce));
            c.sessionrow = run("INSERT INTO sessions(session, address, started) VALUES(?, ?, ?)", "ssi", "", c.addressname, (long long)(nowus()/1000));
            vector<uchar> hello, out;
            sbr2::byteswriter w(hello);
            w.u8(WITNESSPROTO);
            w.bytes(c.session, sizeof(c.session));
            w.bytes(c.nonce, sizeof(c.nonce));
            w.u64(nowus());
            w.u32(newseed(c));
            if(!c.hs->writemessage(hello.getbuf(), hello.length(), out)) { c.closing = true; return; }
            frame(c, out);
            return;
        }
        payload.add('\0');
        copystring(c.build, (const char *)payload.getbuf());
        c.hs->split(c.send, c.recv);
        sbr2::installkeyid(c.hs->remotestatic(), c.installid);
        tohexstr(c.installid, sizeof(c.installid), c.installhex);
        string sessionhex;
        tohexstr(c.session, sizeof(c.session), sessionhex);
        string noncehex;
        tohexstr(c.nonce, sizeof(c.nonce), noncehex);
        run("UPDATE sessions SET session = ?, nonce = ?, installkey = ?, build = ? WHERE id = ?", "ssssi", sessionhex, noncehex, c.installhex, c.build, c.sessionrow);
        DELETEP(c.hs);
        c.open = true;
        conoutf("%s: session %s, player %s, %s", c.addressname, sessionhex, c.installhex, c.build);
    }

    static void handleinput(client &c)
    {
        int pos = 0;
        while(!c.closing && c.in.length() - pos >= 2)
        {
            int len = (c.in[pos] << 8) | c.in[pos+1];
            if(c.in.length() - pos - 2 < len) break;
            const uchar *msg = c.in.getbuf() + pos + 2;
            if(!c.open) handshakemessage(c, msg, len);
            else
            {
                vector<uchar> plain;
                if(!c.recv.decrypt(NULL, 0, msg, len, plain) || plain.empty()) { conoutf("%s: damaged or replayed message", c.addressname); c.closing = true; break; }
                handlemessage(c, plain[0], plain.getbuf() + 1, plain.length() - 1);
            }
            pos += 2 + len;
        }
        c.in.remove(0, pos);
        if(c.in.length() > 2*noise::MAXMESSAGE) c.closing = true;
    }

    static void dropclient(int i)
    {
        client &c = *clients[i];
        loopvj(c.runs) if(!c.runs[j]->done)
        {
            witnessrun &r = *c.runs[j];
            DELETEP(r.file);
            liveabandon(r);
            run("UPDATE runs SET stop = 'disconnected', violations = ?, maxlagms = ?, ended = ? WHERE id = ?", "iiii", (long long)r.violations, (long long)r.maxlagms, (long long)(nowus()/1000), r.row);
        }
        if(c.sessionrow >= 0) run("UPDATE sessions SET ended = ? WHERE id = ?", "ii", (long long)(nowus()/1000), c.sessionrow);
        releaseidle(c.sessionrow);
        conoutf("%s: disconnected", c.addressname);
        delete clients.remove(i);
    }

    // Re-simulations, one sauer_resim -L process each, playing one or more runs in turn. Its
    // stdin says when the file it plays has grown ("more") and when it is final ("end"); after a
    // run's JSON line it waits for the next run of the same life, which it plays on from the
    // state the run ended in ("next\t<file>\t<json file>"), or for EOF. A live job plays runs
    // while they stream; a batch job plays finished files (one batch job at a time, from
    // resimqueue), a run that continues others after them.
    struct jobrun
    {
        long long row;
        string out;
        bool judge, ended;
        unsigned long long since; // when the run ended, or when its batch job started
    };
    struct resimjob
    {
        pid_t pid;
        int input;               // the write end of its stdin, -1 once closed
        bool live;
        long long session, lastrow; // live: the session it plays for, and the last run it was given
        vector<jobrun> runs;     // waiting for their JSON line, in order
        unsigned long long idle; // live: since when it has had no run to play, 0 while it has one
    };
    static vector<resimjob *> resimjobs;
    static int maxlive = 4;
    enum { CHAINWAIT = 5 }; // seconds a live job waits for the next run of the life

    static bool jsonfield(const char *json, const char *key, char *out, int outlen)
    {
        defformatstring(pattern, "\"%s\":", key);
        const char *p = strstr(json, pattern);
        if(!p) return false;
        p += strlen(pattern);
        int n = 0;
        if(*p == '"') { p++; while(p[n] && p[n] != '"' && n < outlen-1) n++; }
        else while(p[n] && p[n] != ',' && p[n] != '}' && n < outlen-1) n++;
        memcpy(out, p, n);
        out[n] = '\0';
        return true;
    }

    static bool rowtext(const char *sql, long long row, int columns, char (*out)[MAXSTRLEN])
    {
        sqlite3_stmt *st = NULL;
        if(sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return false;
        sqlite3_bind_int64(st, 1, row);
        bool found = sqlite3_step(st) == SQLITE_ROW;
        if(found) loopi(columns) { const unsigned char *t = sqlite3_column_text(st, i); copystring(out[i], t ? (const char *)t : ""); }
        sqlite3_finalize(st);
        return found;
    }

    static resimjob *findjob(pid_t pid)
    {
        loopv(resimjobs) if(resimjobs[i]->pid == pid) return resimjobs[i];
        return NULL;
    }

    static void jobout(long long row, char *out)
    {
        nformatstring(out, MAXSTRLEN, "%s/resimhome/resim-%lld.json", datadir, row);
    }

    static bool runfile(long long row, char *file)
    {
        string f[1];
        if(!rowtext("SELECT file FROM runs WHERE id = ?", row, 1, f)) return false;
        copystring(file, f[0], MAXSTRLEN);
        return true;
    }

    static resimjob *startjob(const char *file, const char *out, bool live)
    {
        defformatstring(home, "%s/resimhome", datadir);
        mkdir(home, 0700);
        uchar pub[32];
        sbr2::publickey(signingseed, pub);
        string pubhex;
        tohexstr(pub, 32, pubhex);
        defformatstring(qarg, "-q%s", home);
        defformatstring(oarg, "-o%s", resimoverrides);
        defformatstring(varg, "-V%s", pubhex);
        int fds[2] = { -1, -1 };
        if(pipe(fds) < 0) return NULL;
        pid_t pid = fork();
        if(pid < 0) { close(fds[0]); close(fds[1]); return NULL; }
        if(!pid)
        {
            struct rlimit cpu = { RESIMCPU, RESIMCPU };
            setrlimit(RLIMIT_CPU, &cpu);
            setpriority(PRIO_PROCESS, 0, 10);
            int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600), null = open("/dev/null", O_RDWR);
            dup2(fds[0], 0);
            close(fds[0]);
            close(fds[1]);
            if(fd >= 0) dup2(fd, 1);
            if(null >= 0) dup2(null, 2);
            if(chdir(resimdata) < 0) _exit(3);
            const char *argv[8];
            int n = 0;
            argv[n++] = resimexe;
            argv[n++] = qarg;
            if(resimoverrides[0]) argv[n++] = oarg;
            argv[n++] = varg;
            argv[n++] = "-L";
            argv[n++] = file;
            argv[n] = NULL;
            execv(resimexe, (char * const *)argv);
            _exit(3);
        }
        close(fds[0]);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        resimjob *job = new resimjob;
        job->pid = pid;
        job->input = fds[1];
        job->live = live;
        job->session = job->lastrow = -1;
        job->idle = 0;
        resimjobs.add(job);
        return job;
    }

    static void addrun(resimjob &job, long long row, bool judge, bool ended)
    {
        jobrun &r = job.runs.add();
        r.row = row;
        jobout(row, r.out);
        r.judge = judge;
        r.ended = ended;
        r.since = nowus();
        job.lastrow = row;
        job.idle = 0;
    }

    static int livejobs()
    {
        int n = 0;
        loopv(resimjobs) if(resimjobs[i]->live) n++;
        return n;
    }

    static void tell(resimjob &job, const char *line)
    {
        if(job.input >= 0 && write(job.input, line, strlen(line)) < 0) {}
    }

    static void closeinput(resimjob &job)
    {
        if(job.input >= 0) close(job.input);
        job.input = -1;
    }

    static void tellnext(resimjob &job, long long row)
    {
        string file, out;
        if(!runfile(row, file)) return;
        jobout(row, out);
        defformatstring(line, "next\t%s\t%s\n", file, out);
        tell(job, line);
    }

    // a finished run, after the runs it continues, in one process
    static void startbatch(long long row)
    {
        vector<long long> chain;
        for(long long at = row; at >= 0 && chain.length() < 10000;)
        {
            chain.add(at);
            string follows[1];
            at = rowtext("SELECT follows FROM runs WHERE id = ?", at, 1, follows) && follows[0][0] ? atoll(follows[0]) : -1;
        }
        string file, out;
        if(!runfile(chain.last(), file)) return;
        jobout(chain.last(), out);
        resimjob *job = startjob(file, out, false);
        if(!job) return;
        for(int i = chain.length()-1; i >= 0; i--)
        {
            if(i < chain.length()-1) tellnext(*job, chain[i]);
            tell(*job, "end\n");
            addrun(*job, chain[i], i == 0, true);
        }
        closeinput(*job);
    }

    static void livestart(client &c, witnessrun &r)
    {
        if(!resimexe[0] || r.done || !r.file || r.follows >= 0 || livejobs() >= maxlive) return;
        r.file->flush();
        string file, out;
        if(!runfile(r.row, file)) return;
        jobout(r.row, out);
        resimjob *job = startjob(file, out, true);
        if(!job) return;
        job->session = c.sessionrow;
        addrun(*job, r.row, true, false);
        r.livepid = job->pid;
    }

    // the next run of a life: the job that played the run it continues plays it on
    static void livecontinue(client &c, witnessrun &r)
    {
        loopv(resimjobs)
        {
            resimjob &job = *resimjobs[i];
            if(!job.live || job.input < 0 || job.session != c.sessionrow || job.lastrow != r.follows) continue;
            if(r.file) r.file->flush();
            tellnext(job, r.row);
            addrun(job, r.row, true, false);
            r.livepid = job.pid;
            return;
        }
    }

    // a live job that has played its runs isn't kept for a session that starts something else
    static void releaseidle(long long session)
    {
        loopv(resimjobs) if(resimjobs[i]->live && resimjobs[i]->session == session && resimjobs[i]->runs.empty()) closeinput(*resimjobs[i]);
    }

    static jobrun *livejobrun(witnessrun &r, resimjob *&job)
    {
        job = r.livepid ? findjob(r.livepid) : NULL;
        if(job) loopv(job->runs) if(job->runs[i].row == r.row) return &job->runs[i];
        return NULL;
    }

    static void livemore(witnessrun &r)
    {
        resimjob *job = NULL;
        if(!livejobrun(r, job) || !r.file) return;
        r.file->flush();
        tell(*job, "more\n");
    }

    // after the file is final: closed, with the witness's signatures
    static void liveend(witnessrun &r)
    {
        resimjob *job = NULL;
        jobrun *run = livejobrun(r, job);
        r.livepid = 0;
        if(!run) { if(resimexe[0]) resimqueue.add(r.row); return; }
        tell(*job, "end\n");
        run->ended = true;
        run->since = nowus();
    }

    // the run stopped streaming: its job ends once it has written the runs before it
    static void liveabandon(witnessrun &r)
    {
        resimjob *job = NULL;
        jobrun *run = livejobrun(r, job);
        r.livepid = 0;
        if(!run) return;
        job->runs.remove(int(run - job->runs.getbuf()));
        closeinput(*job);
    }

    static client *sessionclient(long long sessionrow)
    {
        loopv(clients) if(clients[i]->sessionrow == sessionrow && clients[i]->open) return clients[i];
        return NULL;
    }

    // the verdict on a finished run, signed over its content hash
    static void finishresim(const jobrun &job, bool live, int status, bool timedout)
    {
        long long row = job.row;
        char json[8192] = "";
        stream *in = openrawfile(job.out, "rb");
        if(in) { size_t n = in->read(json, sizeof(json) - 1); json[n] = '\0'; delete in; }
        string ok = "", desync = "", runtime = "", recorded = "", failed = "", missing = "", lifestart = "", ticks = "";
        bool read = jsonfield(json, "ok", ok, sizeof(ok)) && jsonfield(json, "rules_failed", failed, sizeof(failed));
        jsonfield(json, "desync_tick", desync, sizeof(desync));
        jsonfield(json, "runtime_ms", runtime, sizeof(runtime));
        jsonfield(json, "recorded_runtime_ms", recorded, sizeof(recorded));
        jsonfield(json, "missing_files", missing, sizeof(missing));
        jsonfield(json, "lifestart", lifestart, sizeof(lifestart));
        jsonfield(json, "ticks", ticks, sizeof(ticks));
        string failednames = "";
        jsonfield(json, "failed", failednames, sizeof(failednames));
        string info[7];
        if(!rowtext("SELECT r.runid, r.content, r.violations, r.seedsok, r.session, s.session, s.nonce || ' ' || r.installkey FROM runs r JOIN sessions s ON s.id = r.session WHERE r.id = ?", row, 7, info)) return;
        const char *verdict = "valid";
        string reason = "", follows[1] = { "" }, before[2] = { "", "" };
        rowtext("SELECT follows FROM runs WHERE id = ?", row, 1, follows);
        if(timedout || !read || (WIFEXITED(status) && WEXITSTATUS(status) >= 2))
        {
            verdict = "unverified";
            copystring(reason, timedout ? "re-simulation timed out" : "re-simulation could not play the file");
        }
        else if(atoi(missing) > 0) { verdict = "unverified"; copystring(reason, "a map file is not available"); }
        else if(strstr(failednames, "data")) { verdict = "rejected"; copystring(reason, "the map files differ from the witness's copies"); }
        else if(strcmp(ok, "true"))
        {
            verdict = "rejected";
            if(atoi(desync)) formatstring(reason, "the replay desynced at tick %s", desync);
            else formatstring(reason, "the re-simulated run (%s ms) differs from the recorded one (%s ms)", runtime, recorded);
        }
        else if(atoi(failed) > 0) { verdict = "rejected"; formatstring(reason, "rules: %s", failednames); }
        else if(!strcmp(lifestart, "chained") != (follows[0][0] != 0)) { verdict = "unverified"; copystring(reason, "the run it continues was not re-simulated before it"); }
        else if(strcmp(lifestart, "ok") && strcmp(lifestart, "skipped") && strcmp(lifestart, "chained")) { verdict = "rejected"; formatstring(reason, "life start differs: %s", lifestart); }
        else if(atoi(info[2]) > 0) { verdict = "rejected"; copystring(reason, "not played in real time"); }
        else if(!atoi(info[3])) { verdict = "rejected"; copystring(reason, "a seed did not come from the witness"); }

        if(follows[0][0] && !strcmp(verdict, "valid") && rowtext("SELECT verdict, reason FROM runs WHERE id = ?", atoll(follows[0]), 2, before) && strcmp(before[0], "valid"))
        {
            verdict = !strcmp(before[0], "rejected") ? "rejected" : "unverified";
            formatstring(reason, "the run it continues is %s", before[0][0] ? before[0] : "not judged");
        }

        sbr2::signature sig;
        sig.scope = sbr2::signature::VERDICT;
        char *space = strchr(info[6], ' ');
        if(space) *space = '\0';
        sbr2::fromhex(info[5], sig.session, sizeof(sig.session));
        sbr2::fromhex(info[6], sig.nonce, sizeof(sig.nonce));
        if(space) sbr2::fromhex(space + 1, sig.installkey, sizeof(sig.installkey));
        sbr2::fromhex(info[1], sig.hash, sbr2::HASHSIZE);
        sig.counter = ++signcounter;
        sig.recvus = nowus();
        sbr2::kvwriter kv;
        kv.str("verdict", verdict);
        if(reason[0]) kv.str("reason", reason);
        kv.var("runtimems", atoi(runtime));
        kv.var("ticks", atoi(ticks));
        kv.build(sig.extras);
        sig.sign(signingseed);
        vector<uchar> payload;
        sig.write(payload);
        run("INSERT INTO signatures(run, scope, segment, recvus, payload) VALUES(?, ?, 0, ?, ?)", "iiib", row, (long long)sig.scope, (long long)sig.recvus, payload.getbuf(), payload.length());
        run("UPDATE runs SET verdict = ?, reason = ?, resim = ?, judged = ? WHERE id = ?", "sssii", verdict, reason, json, (long long)(nowus()/1000), row);
        string file[1];
        if(strcmp(verdict, "unverified") && rowtext("SELECT file FROM runs WHERE id = ?", row, 1, file))
        {
            stream *f = openrawfile(file[0], "ab");
            if(f) { vector<uchar> chunk; sbr2::appendchunk(chunk, "sIGN", payload); f->write(chunk.getbuf(), chunk.length()); delete f; }
        }
        client *c = sessionclient(atoll(info[4]));
        if(c)
        {
            vector<uchar> msg;
            sbr2::byteswriter w(msg);
            w.var(atoi(info[0]));
            w.bytes(payload.getbuf(), payload.length());
            sendmsg(*c, WM_SIGN, msg);
        }
        conoutf("run %lld (session %s, run %s): %s%s%s (%s, %.1f s)", row, info[4], info[0], verdict, reason[0] ? ": " : "", reason,
            live ? "live, after the end" : "re-simulated", (nowus() - job.since) / 1e6);
    }

    static void forgetlive(pid_t pid)
    {
        loopv(clients) loopvj(clients[i]->runs) if(clients[i]->runs[j]->livepid == pid) clients[i]->runs[j]->livepid = 0;
    }

    static bool jsonready(const char *out)
    {
        stream *f = openrawfile(out, "rb");
        if(!f) return false;
        stream::offset size = f->size();
        char last = 0;
        bool ready = size > 0 && f->seek(size - 1, SEEK_SET) && f->read(&last, 1) == 1 && last == '\n';
        delete f;
        return ready;
    }

    static void harvest(resimjob &job)
    {
        while(job.runs.length() && job.runs[0].ended && jsonready(job.runs[0].out))
        {
            jobrun run = job.runs.remove(0);
            if(run.judge) finishresim(run, job.live, 0, false);
            if(job.runs.empty()) job.idle = nowus();
        }
    }

    static void resimslice()
    {
        bool batch = false;
        loopv(resimjobs)
        {
            resimjob &job = *resimjobs[i];
            harvest(job);
            int status = 0;
            bool exited = waitpid(job.pid, &status, WNOHANG) == job.pid, timedout = false;
            loopvj(job.runs) if(job.runs[j].ended && nowus() - job.runs[j].since > (unsigned long long)RESIMWALL * 1000000) timedout = true;
            if(!exited && !timedout)
            {
                if(!job.live) batch = true;
                else if(job.input >= 0 && job.idle && nowus() - job.idle > (unsigned long long)CHAINWAIT * 1000000) closeinput(job);
                continue;
            }
            if(timedout && !exited) { kill(job.pid, SIGKILL); waitpid(job.pid, &status, 0); }
            if(!timedout) harvest(job);
            forgetlive(job.pid);
            loopvj(job.runs)
            {
                const jobrun &run = job.runs[j];
                // a live job that stopped leaves its runs to batch jobs, once they have ended
                if(job.live) { if(run.ended) { conoutf("run %lld: live re-simulation failed, re-simulating the finished file", run.row); resimqueue.add(run.row); } }
                else if(run.judge) finishresim(run, false, status, timedout);
            }
            closeinput(job);
            delete resimjobs.remove(i--);
        }
        if(!batch && resimqueue.length()) startbatch(resimqueue.remove(0));
    }

    // all: every finished run (after a change to the rules), otherwise those without a verdict
    static void queueunjudged(bool all)
    {
        sqlite3_stmt *st = NULL;
        const char *sql = all ? "SELECT id FROM runs WHERE finished = 1 ORDER BY id" : "SELECT id FROM runs WHERE finished = 1 AND (verdict IS NULL OR verdict = 'unverified') ORDER BY id";
        if(sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return;
        while(sqlite3_step(st) == SQLITE_ROW) resimqueue.add(sqlite3_column_int64(st, 0));
        sqlite3_finalize(st);
        if(resimqueue.length()) conoutf("%d finished runs queued for re-simulation", resimqueue.length());
    }

    static bool loadhexfile(const char *file, uchar *out, int n, bool create)
    {
        stream *f = openrawfile(file, "rb");
        if(f)
        {
            char buf[256] = "";
            f->read(buf, sizeof(buf) - 1);
            delete f;
            char *e = buf + strlen(buf);
            while(e > buf && isspace(uchar(e[-1]))) *--e = '\0';
            return sbr2::fromhex(buf, out, n);
        }
        if(!create || !noise::randombytes(out, n)) return false;
        f = openrawfile(file, "wb");
        if(!f) return false;
        string hex;
        tohexstr(out, n, hex);
        f->printf("%s\n", hex);
        delete f;
        chmod(file, 0600);
        return true;
    }

    static int serve(int port)
    {
        ENetAddress address;
        address.host = ENET_HOST_ANY;
        address.port = port;
        ENetSocket listener = enet_socket_create(ENET_SOCKET_TYPE_STREAM);
        if(listener == ENET_SOCKET_NULL || enet_socket_set_option(listener, ENET_SOCKOPT_REUSEADDR, 1) < 0 ||
           enet_socket_bind(listener, &address) < 0 || enet_socket_listen(listener, 16) < 0)
        {
            conoutf("could not listen on port %d", port);
            return 1;
        }
        enet_socket_set_option(listener, ENET_SOCKOPT_NONBLOCK, 1);
        uchar pub[noise::KEYLEN];
        noise::publickey(channelkey, pub);
        string pubhex;
        tohexstr(pub, noise::KEYLEN, pubhex);
        uchar signpub[32];
        sbr2::publickey(signingseed, signpub);
        string signhex;
        tohexstr(signpub, 32, signhex);
        conoutf("witness listening on port %d; channel key %s; signing key %s", port, pubhex, signhex);
        const uchar prologue[] = "speedbraten/1 witness";
        for(;;)
        {
            ENetSocketSet readset, writeset;
            ENET_SOCKETSET_EMPTY(readset);
            ENET_SOCKETSET_EMPTY(writeset);
            ENetSocket maxsock = listener;
            ENET_SOCKETSET_ADD(readset, listener);
            loopv(clients)
            {
                client &c = *clients[i];
                ENET_SOCKETSET_ADD(readset, c.sock);
                if(c.out.length()) ENET_SOCKETSET_ADD(writeset, c.sock);
                maxsock = max(maxsock, c.sock);
            }
            if(resimexe[0]) resimslice();
            if(enet_socketset_select(maxsock, &readset, &writeset, resimjobs.length() || resimqueue.length() ? 100 : 1000) < 0) continue;
            int now = int(nowus() / 1000);
            if(ENET_SOCKETSET_CHECK(readset, listener))
            {
                ENetAddress from;
                ENetSocket s = enet_socket_accept(listener, &from);
                int same = 0;
                loopv(clients) if(clients[i]->address.host == from.host) same++;
                if(s != ENET_SOCKET_NULL && (clients.length() >= MAXCONNECTIONS || same >= MAXPERADDRESS)) { enet_socket_destroy(s); s = ENET_SOCKET_NULL; }
                if(s != ENET_SOCKET_NULL)
                {
                    client *c = clients.add(new client);
                    c->sock = s;
                    c->address = from;
                    c->connected = now;
                    enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
                    char ip[64] = "";
                    enet_address_get_host_ip(&from, ip, sizeof(ip));
                    formatstring(c->addressname, "%s:%d", ip, from.port);
                    c->hs = new noise::handshake(false, prologue, sizeof(prologue) - 1, channelkey, NULL);
                }
            }
            loopv(clients)
            {
                client &c = *clients[i];
                if(ENET_SOCKETSET_CHECK(readset, c.sock))
                {
                    uchar buf[65536];
                    ENetBuffer eb;
                    eb.data = buf;
                    eb.dataLength = sizeof(buf);
                    int n = enet_socket_receive(c.sock, NULL, &eb, 1);
                    if(n <= 0) c.closing = true;
                    else { c.in.put(buf, n); handleinput(c); }
                }
                if(c.out.length() && ENET_SOCKETSET_CHECK(writeset, c.sock))
                {
                    ENetBuffer eb;
                    eb.data = c.out.getbuf();
                    eb.dataLength = c.out.length();
                    int n = enet_socket_send(c.sock, NULL, &eb, 1);
                    if(n < 0) c.closing = true;
                    else c.out.remove(0, n);
                }
                if(!c.open && now - c.connected > HANDSHAKETIMEOUT) c.closing = true;
                if(c.closing && (!c.out.length() || !c.open)) dropclient(i--);
            }
        }
        return 0;
    }
}

int main(int argc, char **argv)
{
    using namespace witness;
    const char *seedfile = NULL, *keyfile = NULL;
    int port = WITNESSPORT;
    bool rejudge = false;
    for(int i = 1; i < argc; i++) if(argv[i][0] == '-') switch(argv[i][1])
    {
        case 'd': copystring(datadir, &argv[i][2]); break;
        case 's': seedfile = &argv[i][2]; break;
        case 'c': keyfile = &argv[i][2]; break;
        case 'p': port = atoi(&argv[i][2]); break;
        case 'e': copystring(resimexe, &argv[i][2]); break;
        case 'r': copystring(resimdata, &argv[i][2]); break;
        case 'o': copystring(resimoverrides, &argv[i][2]); break;
        case 'R': rejudge = true; break;
        case 'j': maxlive = max(atoi(&argv[i][2]), 0); break;
    }
    if(!datadir[0] || !seedfile) fatal("usage: sauer_witness -d<data dir> -s<signing seed file> [-c<channel key file>] [-p<port>] [-e<sauer_resim> -r<game data dir> [-o<overrides>] [-j<live re-simulations>] [-R]]");
    mkdir(datadir, 0700);
    defformatstring(runsdir, "%s/runs", datadir);
    mkdir(runsdir, 0700);
    if(!loadhexfile(seedfile, signingseed, 32, false)) fatal("could not read the signing seed from %s", seedfile);
    defformatstring(defaultkey, "%s/channel.key", datadir);
    if(!loadhexfile(keyfile ? keyfile : defaultkey, channelkey, noise::KEYLEN, true)) fatal("could not read or create the channel key");
    if(!opendb()) fatal("could not open %s/witness.db", datadir);
    if(resimexe[0] && !resimdata[0]) fatal("-e needs -r<game data dir> to run sauer_resim in");
    if(datadir[0] != '/') { char cwd[4096]; if(getcwd(cwd, sizeof(cwd))) { defformatstring(abs, "%s/%s", cwd, datadir); copystring(datadir, abs); } }
    if(resimexe[0]) queueunjudged(rejudge);
    signal(SIGPIPE, SIG_IGN);
    if(enet_initialize() < 0) fatal("could not initialise networking");
    return serve(port);
}
