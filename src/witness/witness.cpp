// sauer_witness: receives replays while they are played and signs what it received. Clients
// connect over the Noise channel (shared/noise.h, messages in shared/witnessproto.h) and stream
// each run's replay file as it is written. For every run the witness
//   - parses the bytes as they arrive (sbr2::streamreader) and keeps them in a file,
//   - checks that every map attempt uses a seed it handed out,
//   - checks every TICK commitment against the state hash stored for that tick,
//   - bounds how far the run's ticks fall behind or run ahead of its own clock,
//   - signs each segment's chain head as it closes and the content hash once END arrives.
// Sessions, runs, seeds and signatures are recorded in witness.db (SQLite).
//
//   sauer_witness -d<data dir> -s<signing seed file> [-c<channel key file>] [-p<port>]
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

    static uchar channelkey[noise::KEYLEN], signingseed[32];
    static string datadir = "";
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
            "CREATE TABLE IF NOT EXISTS signatures(run INTEGER, scope INTEGER, segment INTEGER, recvus INTEGER, payload BLOB);");
    }

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

        witnessrun() : row(-1), runid(0), kind(0), starttick(0), maxlagms(0), violations(0), inpslo(0), segticks(0), t0(0), seedsok(true), done(false), seenchunks(0), bytes(0), file(NULL) { refused[0] = '\0'; }
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

    static void refuse(client &c, witnessrun &r, const char *why)
    {
        if(r.refused[0] || r.done) return;
        copystring(r.refused, why);
        r.done = true;
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
            int found = c.seeds.find(seed);
            if(!b.ok || found < 0) { r.seedsok = false; run("UPDATE runs SET seedsok = 0 WHERE id = ?", "i", r.row); }
            else if(!c.seedsused[found]) { c.seedsused[found] = true; run("UPDATE seeds SET used = ? WHERE session = ? AND seed = ?", "iii", (long long)(nowus()/1000), c.sessionrow, (long long)seed); sendseed(c); }
            r.segticks = 0;
        }
        else if(!memcmp(ch.type, "INPS", 4))
        {
            r.inpslo = int(b.var());
            r.segticks = int(b.var());
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
            r.done = true;
            string hex;
            tohexstr(r.reader.content, sbr2::HASHSIZE, hex);
            run("UPDATE runs SET finished = 1, content = ?, violations = ?, maxlagms = ?, ended = ? WHERE id = ?", "siiii", hex, (long long)r.violations, (long long)r.maxlagms, (long long)(nowus()/1000), r.row);
            if(r.file)
            {
                loopv(r.signatures) { vector<uchar> chunk; sbr2::appendchunk(chunk, "sIGN", r.signatures[i]); r.file->write(chunk.getbuf(), chunk.length()); }
                DELETEP(r.file);
            }
            conoutf("%s: run %d signed: %d segments, %d bytes, %s, %d violations, max lag %d ms", c.addressname, r.runid, r.reader.segment, r.bytes, r.seedsok ? "witness seeds" : "unknown seed", r.violations, r.maxlagms);
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
                r = c.runs.add(new witnessrun);
                r->runid = runid;
                r->kind = int(b.var());
                r->starttick = int(b.var());
                r->t0 = nowus();
                defformatstring(dir, "%s/runs/%s", datadir, c.installhex);
                mkdir(dir, 0755);
                defformatstring(file, "%s/%lld-%d.sbr", dir, c.sessionrow, runid);
                r->file = openrawfile(file, "wb");
                r->row = run("INSERT INTO runs(session, runid, installkey, kind, starttick, started, file) VALUES(?, ?, ?, ?, ?, ?, ?)", "iisiiis",
                    c.sessionrow, (long long)runid, c.installhex, (long long)r->kind, (long long)r->starttick, (long long)(r->t0/1000), file);
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
                r->done = true;
                DELETEP(r->file);
                run("UPDATE runs SET stop = ?, violations = ?, maxlagms = ?, ended = ? WHERE id = ?", "siiii", reason, (long long)r->violations, (long long)r->maxlagms, (long long)(nowus()/1000), r->row);
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
        run("UPDATE sessions SET session = ?, installkey = ?, build = ? WHERE id = ?", "sssi", sessionhex, c.installhex, c.build, c.sessionrow);
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
            run("UPDATE runs SET stop = 'disconnected', violations = ?, maxlagms = ?, ended = ? WHERE id = ?", "iiii", (long long)r.violations, (long long)r.maxlagms, (long long)(nowus()/1000), r.row);
        }
        if(c.sessionrow >= 0) run("UPDATE sessions SET ended = ? WHERE id = ?", "ii", (long long)(nowus()/1000), c.sessionrow);
        conoutf("%s: disconnected", c.addressname);
        delete clients.remove(i);
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
            if(enet_socketset_select(maxsock, &readset, &writeset, 1000) < 0) continue;
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
    for(int i = 1; i < argc; i++) if(argv[i][0] == '-') switch(argv[i][1])
    {
        case 'd': copystring(datadir, &argv[i][2]); break;
        case 's': seedfile = &argv[i][2]; break;
        case 'c': keyfile = &argv[i][2]; break;
        case 'p': port = atoi(&argv[i][2]); break;
    }
    if(!datadir[0] || !seedfile) fatal("usage: sauer_witness -d<data dir> -s<signing seed file> [-c<channel key file>] [-p<port>]");
    mkdir(datadir, 0700);
    defformatstring(runsdir, "%s/runs", datadir);
    mkdir(runsdir, 0700);
    if(!loadhexfile(seedfile, signingseed, 32, false)) fatal("could not read the signing seed from %s", seedfile);
    defformatstring(defaultkey, "%s/channel.key", datadir);
    if(!loadhexfile(keyfile ? keyfile : defaultkey, channelkey, noise::KEYLEN, true)) fatal("could not read or create the channel key");
    if(!opendb()) fatal("could not open %s/witness.db", datadir);
    if(enet_initialize() < 0) fatal("could not initialise networking");
    return serve(port);
}
