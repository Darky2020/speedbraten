// Client side of the encrypted channels to the speedbraten servers (witness and hub): a TCP
// connection carrying Noise_XK_25519_ChaChaPoly_BLAKE2b (shared/noise.h) with the server's
// static key pinned. Nothing here blocks: the host name is resolved on its own thread, the
// socket is non-blocking and witnessslice() moves the connection along once per frame.
//
// Framing matches server/noisechan: a big-endian u16 length, then a Noise message; a transport
// message's plaintext is a u8 type followed by the payload. Opened without a server key, a
// connection asks for the server's public keys instead (an empty first frame) and closes: the
// witness answers "SBRW", 1, its channel and signing keys; the hub "SBRH", 1, its channel key.

#include "engine.h"
#include "noise.h"

namespace witnessnet
{
    enum { IDLE = 0, RESOLVING, CONNECTING, HANDSHAKE, OPEN, CLOSED };
    enum { CONNECTTIMEOUT = 10000, MAXQUEUED = 4<<20 };

    // a lookup on its own thread; whoever finishes last (the thread or the connection) frees it
    struct lookup
    {
        string host;
        ENetAddress address;
        bool done, ok, abandoned;
    };
    static SDL_mutex *lookupmutex = NULL;

    static int lookupthread(void *data)
    {
        lookup *l = (lookup *)data;
        ENetAddress address;
        address.port = 0;
        bool ok = enet_address_set_host(&address, l->host) >= 0;
        SDL_LockMutex(lookupmutex);
        bool abandoned = l->abandoned;
        l->address.host = address.host;
        l->ok = ok;
        l->done = true;
        SDL_UnlockMutex(lookupmutex);
        if(abandoned) delete l;
        return 0;
    }

    struct received
    {
        int type;
        vector<uchar> payload;
    };

    struct connection
    {
        int state, started;
        string host, error;
        int port;
        uchar serverkey[noise::KEYLEN];
        bool askkeys, learned, hub;
        uchar keys[2*noise::KEYLEN]; // what a key request got: the channel key, then the witness's signing key
        vector<uchar> prologue, hello;
        lookup *pending;
        ENetSocket sock;
        noise::handshake *hs;
        noise::cipherstate send, recv;
        vector<uchar> in, out;
        vector<received *> inbox;

        connection() : state(IDLE), started(0), port(0), askkeys(false), learned(false), hub(false), pending(NULL), sock(ENET_SOCKET_NULL), hs(NULL) { host[0] = error[0] = '\0'; }
        ~connection() { close(""); }

        void close(const char *why)
        {
            if(pending)
            {
                SDL_LockMutex(lookupmutex);
                bool done = pending->done;
                pending->abandoned = true;
                SDL_UnlockMutex(lookupmutex);
                if(done) delete pending;
                pending = NULL;
            }
            if(sock != ENET_SOCKET_NULL) { enet_socket_destroy(sock); sock = ENET_SOCKET_NULL; }
            DELETEP(hs);
            send.wipe();
            recv.wipe();
            in.setsize(0);
            out.setsize(0);
            inbox.deletecontents();
            if(why[0] && state != CLOSED) copystring(error, why);
            state = why[0] ? CLOSED : IDLE;
        }

        void fail(const char *why) { logoutf("%s connection to %s:%d: %s", hub ? "hub" : "witness", host, port, why); close(why); }

        bool frame(const vector<uchar> &msg)
        {
            if(msg.length() > noise::MAXMESSAGE || out.length() + msg.length() + 2 > MAXQUEUED) return false;
            out.add(uchar(msg.length() >> 8));
            out.add(uchar(msg.length()));
            out.put(msg.getbuf(), msg.length());
            return true;
        }

        bool sendmessage(int type, const uchar *data, int len)
        {
            if(state != OPEN || len + 1 + noise::TAGLEN > noise::MAXMESSAGE) return false;
            vector<uchar> plain, msg;
            plain.add(uchar(type));
            plain.put(data, len);
            if(!send.encrypt(NULL, 0, plain.getbuf(), plain.length(), msg) || !frame(msg)) { fail("send queue full"); return false; }
            return true;
        }

        void startsocket(const ENetAddress &resolved)
        {
            ENetAddress address = resolved;
            address.port = port;
            sock = enet_socket_create(ENET_SOCKET_TYPE_STREAM);
            if(sock == ENET_SOCKET_NULL) { fail("no socket"); return; }
            enet_socket_set_option(sock, ENET_SOCKOPT_NONBLOCK, 1);
            enet_socket_set_option(sock, ENET_SOCKOPT_NODELAY, 1);
            if(enet_socket_connect(sock, &address) < 0) { fail("could not connect"); return; }
            state = CONNECTING;
        }

        void begin()
        {
            if(askkeys)
            {
                vector<uchar> none;
                if(!frame(none)) { fail("key request"); return; }
                state = HANDSHAKE;
                return;
            }
            uchar installkey[noise::KEYLEN];
            if(!loadinstallkey(installkey)) { fail("no install key"); return; }
            hs = new noise::handshake(true, prologue.getbuf(), prologue.length(), installkey, serverkey);
            memset(installkey, 0, sizeof(installkey));
            vector<uchar> msg;
            if(!hs->writemessage(NULL, 0, msg) || !frame(msg)) { fail("handshake"); return; }
            state = HANDSHAKE;
        }

        void handle(const uchar *msg, int len)
        {
            if(state == HANDSHAKE && askkeys)
            {
                int n = hub ? 1 : 2;
                if(len != 5 + n*noise::KEYLEN || memcmp(msg, hub ? "SBRH" : "SBRW", 4) || msg[4] != 1) { fail("the server didn't send its keys"); return; }
                memset(keys, 0, sizeof(keys));
                memcpy(keys, msg + 5, n*noise::KEYLEN);
                learned = true;
                close("");
                return;
            }
            if(state == HANDSHAKE)
            {
                vector<uchar> payload;
                if(!hs->readmessage(msg, len, payload)) { fail("handshake refused: wrong server key or a damaged message"); return; }
                vector<uchar> last;
                if(!hs->writemessage(hello.getbuf(), hello.length(), last) || !frame(last) || !hs->split(send, recv)) { fail("handshake"); return; }
                DELETEP(hs);
                state = OPEN;
                received *r = inbox.add(new received);
                r->type = -1;
                r->payload.move(payload);
                return;
            }
            vector<uchar> plain;
            if(!recv.decrypt(NULL, 0, msg, len, plain) || plain.empty()) { fail("damaged or replayed message"); return; }
            received *r = inbox.add(new received);
            r->type = plain[0];
            r->payload.put(plain.getbuf() + 1, plain.length() - 1);
        }

        void slice()
        {
            if(state == RESOLVING)
            {
                SDL_LockMutex(lookupmutex);
                bool done = pending->done, ok = pending->ok;
                ENetAddress address = pending->address;
                SDL_UnlockMutex(lookupmutex);
                if(!done)
                {
                    if(totalmillis - started > CONNECTTIMEOUT) fail("could not look up the host");
                    return;
                }
                delete pending;
                pending = NULL;
                if(!ok) { fail("could not look up the host"); return; }
                startsocket(address);
                if(state != CONNECTING) return;
            }
            if(state == CONNECTING)
            {
                enet_uint32 events = ENET_SOCKET_WAIT_SEND;
                if(enet_socket_wait(sock, &events, 0) < 0) { fail("could not connect"); return; }
                if(!(events & ENET_SOCKET_WAIT_SEND))
                {
                    if(totalmillis - started > CONNECTTIMEOUT) fail("connection timed out");
                    return;
                }
                int err = 0;
                if(enet_socket_get_option(sock, ENET_SOCKOPT_ERROR, &err) < 0 || err) { fail("could not connect"); return; }
                begin();
                if(state != HANDSHAKE) return;
            }
            if(state != HANDSHAKE && state != OPEN) return;
            while(out.length())
            {
                ENetBuffer buf;
                buf.data = out.getbuf();
                buf.dataLength = out.length();
                int n = enet_socket_send(sock, NULL, &buf, 1);
                if(n < 0) { fail("connection lost"); return; }
                if(!n) break;
                out.remove(0, n);
            }
            for(;;)
            {
                enet_uint32 events = ENET_SOCKET_WAIT_RECEIVE;
                if(enet_socket_wait(sock, &events, 0) < 0) { fail("connection lost"); return; }
                if(!(events & ENET_SOCKET_WAIT_RECEIVE)) break;
                uchar chunk[16384];
                ENetBuffer buf;
                buf.data = chunk;
                buf.dataLength = sizeof(chunk);
                int n = enet_socket_receive(sock, NULL, &buf, 1);
                if(n <= 0) { fail(askkeys ? "the server didn't send its keys" : state == HANDSHAKE ? "the server closed the connection during the handshake" : "the server closed the connection"); return; }
                in.put(chunk, n);
                int pos = 0;
                while(in.length() - pos >= 2)
                {
                    int len = (in[pos] << 8) | in[pos+1];
                    if(in.length() - pos - 2 < len) break;
                    handle(in.getbuf() + pos + 2, len);
                    if(state != HANDSHAKE && state != OPEN) return;
                    pos += 2 + len;
                }
                in.remove(0, pos);
            }
            if(state == HANDSHAKE && totalmillis - started > CONNECTTIMEOUT) fail("handshake timed out");
        }

        void open(const char *h, int p, const uchar *key, const char *service, const uchar *hellodata, int hellolen)
        {
            close("");
            copystring(host, h);
            port = p;
            askkeys = !key;
            learned = false;
            hub = !strcmp(service, "hub");
            if(key) memcpy(serverkey, key, noise::KEYLEN);
            prologue.setsize(0);
            prologue.put((const uchar *)"speedbraten/1 ", 14);
            prologue.put((const uchar *)service, strlen(service));
            hello.setsize(0);
            hello.put(hellodata, hellolen);
            error[0] = '\0';
            started = totalmillis;
            ENetAddress address;
            if(enet_address_set_host_ip(&address, host) >= 0) { startsocket(address); return; }
            if(!lookupmutex) lookupmutex = SDL_CreateMutex();
            pending = new lookup;
            copystring(pending->host, host);
            pending->done = pending->ok = pending->abandoned = false;
            SDL_Thread *t = SDL_CreateThread(lookupthread, "witness lookup", pending);
            if(!t) { delete pending; pending = NULL; fail("could not look up the host"); return; }
            SDL_DetachThread(t);
            state = RESOLVING;
        }

        static bool loadinstallkey(uchar *key)
        {
            const char *file = "witnessid.key";
            stream *f = openrawfile(path(file, true), "rb");
            if(f)
            {
                bool ok = f->read(key, noise::KEYLEN) == size_t(noise::KEYLEN);
                delete f;
                if(ok) return true;
            }
            if(!noise::randombytes(key, noise::KEYLEN)) return false;
            f = openrawfile(path(file, true), "wb");
            if(!f) return false;
            bool ok = f->write(key, noise::KEYLEN) == size_t(noise::KEYLEN);
            delete f;
            return ok;
        }
    };

    static connection conn, witness, hub;

    static bool receive(connection &c, int &type, vector<uchar> &payload)
    {
        if(c.inbox.empty()) return false;
        received *r = c.inbox.remove(0);
        type = r->type;
        payload.setsize(0);
        payload.move(r->payload);
        delete r;
        return true;
    }

    static int publicstate(const connection &c)
    {
        switch(c.state)
        {
            case IDLE: return WITNESS_IDLE;
            case OPEN: return WITNESS_OPEN;
            case CLOSED: return WITNESS_CLOSED;
            default: return WITNESS_CONNECTING;
        }
    }
}

// serverkey NULL: ask the server for its keys (witnesslearnedkeys) instead of connecting
void witnessconnect(const char *host, int port, const uchar *serverkey, const char *build)
{
    if(!host || !host[0]) { witnessnet::witness.close(""); return; }
    witnessnet::witness.open(host, port, serverkey, "witness", (const uchar *)build, strlen(build));
}

int witnessstate() { return witnessnet::publicstate(witnessnet::witness); }

const char *witnesserror() { return witnessnet::witness.error; }

bool witnesslearnedkeys(uchar *channelkey, uchar *signingkey)
{
    witnessnet::connection &c = witnessnet::witness;
    if(!c.learned) return false;
    memcpy(channelkey, c.keys, noise::KEYLEN);
    memcpy(signingkey, c.keys + noise::KEYLEN, noise::KEYLEN);
    c.learned = false;
    return true;
}

bool witnesssend(int type, const uchar *data, int len) { return witnessnet::witness.sendmessage(type, data, len); }

bool witnessreceive(int &type, vector<uchar> &payload) { return witnessnet::receive(witnessnet::witness, type, payload); }

void hubconnect(const char *host, int port, const uchar *serverkey, const char *build)
{
    if(!host || !host[0]) { witnessnet::hub.close(""); return; }
    witnessnet::hub.open(host, port, serverkey, "hub", (const uchar *)build, strlen(build));
}

int hubstate() { return witnessnet::publicstate(witnessnet::hub); }
const char *huberror() { return witnessnet::hub.error; }

bool hublearnedkey(uchar *channelkey)
{
    witnessnet::connection &c = witnessnet::hub;
    if(!c.learned) return false;
    memcpy(channelkey, c.keys, noise::KEYLEN);
    c.learned = false;
    return true;
}

bool hubsend(int type, const uchar *data, int len) { return witnessnet::hub.sendmessage(type, data, len); }
bool hubreceive(int &type, vector<uchar> &payload) { return witnessnet::receive(witnessnet::hub, type, payload); }

// The interop check against server/cmd/noiseecho: sends count messages of growing size and
// expects each one back unchanged. Prints NOISEINTEROP lines.
namespace witnessnet
{
    static int interopcount = 0, interopsent = 0, interopgot = 0, interopstart = 0;
    static bool interopactive = false;

    static void interoppayload(int i, vector<uchar> &out)
    {
        out.setsize(0);
        int len = (i * 997) % 20000;
        uint x = uint(i) * 2654435761U + 1;
        loopj(len) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; out.add(uchar(x)); }
    }

    static void interopslice()
    {
        if(!interopactive) return;
        if(conn.state == CLOSED) { conoutf("NOISEINTEROP FAIL closed after %d of %d: %s", interopgot, interopcount, conn.error); interopactive = false; return; }
        if(totalmillis - interopstart > 60000) { conoutf("NOISEINTEROP FAIL timeout after %d of %d", interopgot, interopcount); interopactive = false; conn.close("timeout"); return; }
        while(conn.inbox.length())
        {
            received *r = conn.inbox.remove(0);
            if(r->type < 0)
            {
                if(r->payload.length() != 6 || memcmp(r->payload.getbuf(), "echo/1", 6)) { conoutf("NOISEINTEROP FAIL server hello"); interopactive = false; }
            }
            else
            {
                vector<uchar> want;
                interoppayload(interopgot, want);
                if(r->type != interopgot % 256 || r->payload.length() != want.length() || memcmp(r->payload.getbuf(), want.getbuf(), want.length()))
                {
                    conoutf("NOISEINTEROP FAIL message %d differs", interopgot);
                    interopactive = false;
                }
                interopgot++;
            }
            delete r;
            if(!interopactive) return;
        }
        while(conn.state == OPEN && interopsent < interopcount && interopsent - interopgot < 32)
        {
            vector<uchar> p;
            interoppayload(interopsent, p);
            if(!conn.sendmessage(interopsent % 256, p.getbuf(), p.length())) return;
            interopsent++;
        }
        if(interopgot == interopcount)
        {
            conoutf("NOISEINTEROP ok %d messages in %d ms", interopcount, totalmillis - interopstart);
            interopactive = false;
            conn.close("");
        }
    }

    static void noiseinterop(char *host, int *port, char *keyhex, int *count)
    {
        uchar key[noise::KEYLEN];
        if(strlen(keyhex) != 2*noise::KEYLEN) { conoutf(CON_ERROR, "noiseinterop: the server key must be 64 hex digits"); return; }
        loopi(noise::KEYLEN) { char b[3] = { keyhex[2*i], keyhex[2*i+1], '\0' }; key[i] = uchar(strtoul(b, NULL, 16)); }
        interopcount = max(*count, 0);
        interopsent = interopgot = 0;
        interopstart = totalmillis;
        interopactive = true;
        conn.open(host, *port, key, "test", (const uchar *)"interop", 7);
    }
    COMMAND(noiseinterop, "sisi");
}

void witnessslice()
{
    witnessnet::witness.slice();
    witnessnet::hub.slice();
    witnessnet::conn.slice();
    witnessnet::interopslice();
}
