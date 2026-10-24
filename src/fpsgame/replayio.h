// SBR2 replay container: chunks with CRCs, optional zstd compression, and a BLAKE3 hash chain
// closed at every SEGE chunk and summed up in the END chunk.
//
//   file  = "SBR2" var(version) chunk* END-chunk sIGN-chunk*
//   chunk = type[4] var(flags) var(stored length) payload crc32(type..payload)
//
// A chunk type with an uppercase first letter is required: a reader that does not know it must
// refuse the file. flags bit 0: the payload is var(raw length) followed by the next bytes of the
// file's zstd stream, flushed at the chunk's end, so compressed chunks are decompressed in file
// order. From version 2 on the stream restarts after every SEGE, so a reader can start decoding
// at any segment. hashed(c) = type || var(raw length) || raw payload. The chain starts as
// BLAKE3("SBR2-head" || var(version)); every chunk except sIGN and END is hashed into a running
// digest; a SEGE chunk then sets head = BLAKE3("SBR2-seg" || head || digest) and restarts the
// digest; END holds var(segments) || BLAKE3("SBR2-end" || head || digest). Only sIGN chunks may
// follow END.

#ifndef __REPLAYIO_H__
#define __REPLAYIO_H__

namespace sbr2
{
    enum { VERSION = 2, MINVERSION = 1, HASHSIZE = 32, FLAG_ZSTD = 1 };

    struct byteswriter
    {
        vector<uchar> &buf;
        byteswriter(vector<uchar> &b) : buf(b) {}
        void u8(int v) { buf.add(uchar(v)); }
        void u32(uint v) { loopi(4) buf.add(uchar(v >> (i*8))); }
        void u64(unsigned long long v) { loopi(8) buf.add(uchar(v >> (i*8))); }
        void var(unsigned long long v) { do { uchar c = v & 0x7F; v >>= 7; if(v) c |= 0x80; buf.add(c); } while(v); }
        void svar(long long v) { var((unsigned long long)(v) << 1 ^ (unsigned long long)(v >> 63)); }
        void bytes(const void *p, int n) { buf.put((const uchar *)p, n); }
        void str(const char *s) { int n = strlen(s); var(n); bytes(s, n); }
    };

    // Reads with bounds checks; any problem clears ok. Non-minimal varints are an error.
    struct bytesreader
    {
        const uchar *p, *end;
        bool ok;
        bytesreader(const uchar *b, int n) : p(b), end(b + n), ok(true) {}
        int left() const { return int(end - p); }
        int u8() { if(p >= end) { ok = false; return 0; } return *p++; }
        uint u32() { uint v = 0; loopi(4) v |= uint(u8()) << (i*8); return v; }
        unsigned long long u64() { unsigned long long v = 0; loopi(8) v |= (unsigned long long)u8() << (i*8); return v; }
        unsigned long long var()
        {
            unsigned long long v = 0;
            for(int shift = 0; shift < 64; shift += 7)
            {
                int c = u8();
                if(!ok) return 0;
                v |= (unsigned long long)(c & 0x7F) << shift;
                if(!(c & 0x80))
                {
                    if(!c && shift) ok = false;
                    return v;
                }
            }
            ok = false;
            return 0;
        }
        long long svar() { unsigned long long v = var(); return (long long)(v >> 1) ^ -(long long)(v & 1); }
        bool bytes(void *dst, int n) { if(n < 0 || n > left()) { ok = false; return false; } memcpy(dst, p, n); p += n; return true; }
        bool str(char *dst, int size)
        {
            unsigned long long n = var();
            if(!ok || n > (unsigned long long)left() || n >= (unsigned long long)size) { ok = false; dst[0] = '\0'; return false; }
            memcpy(dst, p, n); dst[n] = '\0'; p += n;
            return true;
        }
    };

    enum { VALUE_SVAR = 0, VALUE_VAR, VALUE_STR, VALUE_BYTES, VALUE_F32, VALUE_BOOL };

    // key-value payload: var count, then (str key, u8 value type, value) per entry, keys sorted
    // by bytes and unique
    struct kvwriter
    {
        struct entry
        {
            string key;
            int type;
            vector<uchar> value;
        };
        vector<entry *> entries;

        ~kvwriter() { entries.deletecontents(); }
        void svar(const char *key, long long v) { byteswriter(add(key, VALUE_SVAR)).svar(v); }
        void var(const char *key, unsigned long long v) { byteswriter(add(key, VALUE_VAR)).var(v); }
        void str(const char *key, const char *v) { byteswriter(add(key, VALUE_STR)).str(v); }
        void bytes(const char *key, const uchar *p, int n) { byteswriter w(add(key, VALUE_BYTES)); w.var(n); w.bytes(p, n); }
        void f32(const char *key, float v) { uint bits; memcpy(&bits, &v, 4); byteswriter(add(key, VALUE_F32)).u32(bits); }
        void boolean(const char *key, bool v) { byteswriter(add(key, VALUE_BOOL)).u8(v ? 1 : 0); }
        void build(vector<uchar> &out);
    private:
        vector<uchar> &add(const char *key, int type);
    };

    struct chunk
    {
        char type[5];
        int flags, segment; // segment: index of the segment the chunk belongs to
        vector<uchar> data; // uncompressed payload
    };

    struct writer
    {
        vector<uchar> out;
        int segments;
        uchar head[HASHSIZE];
        writer();
        ~writer();
        // compress: store the payload in the file's zstd stream
        void add(const char *type, const vector<uchar> &payload, bool compress = false);
        void add(const char *type, const uchar *payload, int len, bool compress = false);
        // closes the current segment (a SEGE chunk) and returns the new chain head
        const uchar *endsegment(int endtick);
        // writes END; returns the content hash
        const uchar *finish();
        uchar content[HASHSIZE];
        // zstd level: 19 packs saved files tightest, but its context takes tens of MB and its
        // first chunk several ms, too much for a stream started during play
        int level;
    private:
        void *running, *zstream;
        bool finished;
    };

    struct reader
    {
        vector<chunk *> chunks;
        int version;
        vector<uchar> heads;     // HASHSIZE bytes per closed segment
        uchar content[HASHSIZE]; // recomputed content hash
        bool contentok;          // END holds the recomputed content hash
        string error;
        reader() : version(0) {}
        ~reader() { chunks.deletecontents(); }
        // known: the chunk types this reader understands (4 chars each, concatenated); an unknown
        // required chunk is an error. Returns false with error set on any problem.
        bool parse(const uchar *buf, int len, const char *known);
        const chunk *find(const char *type, int from = 0) const;
    };

    // The same checks as reader, on a file that arrives in pieces: feed() takes the next bytes
    // and keeps every chunk completed so far in chunks (with its segment), the chain head after
    // each SEGE in heads, and the content hash once END has arrived.
    struct streamreader
    {
        int version, segment;
        bool ended;
        vector<chunk *> chunks;
        vector<uchar> heads;
        uchar content[HASHSIZE];
        string error;

        streamreader();
        ~streamreader();
        // false once anything is wrong; error says what
        bool feed(const uchar *data, int len, const char *known);

    private:
        vector<uchar> pending;
        uchar head[HASHSIZE];
        void *running, *zstream;
        bool failed;
        bool fail(const char *msg) { copystring(error, msg); failed = true; return false; }
    };

    void tohex(const uchar *p, int n, char *out);
    // exactly 2*n hex digits into n bytes
    bool fromhex(const char *s, uchar *out, int n);

    // the chunk types replay files of this build understand
    extern const char KNOWNCHUNKS[];

    // appends a chunk to a file's bytes (no hashing: for sIGN after END)
    void appendchunk(vector<uchar> &out, const char *type, const vector<uchar> &payload);

    // A witness signature (sIGN chunk) over a segment's chain head (scope SEGMENT), the content
    // hash as streamed (scope FINAL) or the content hash after re-simulation, with the result in
    // the extras (scope VERDICT). From version 2 on it names the player's install key.
    struct signature
    {
        enum { VERSION = 2, ED25519 = 1 };
        enum { SEGMENT = 0, FINAL = 1, VERDICT = 2 };
        enum { INSTALLKEYID = 16 };
        int version, algorithm, scope, segment;
        uchar keyid[8], session[16], nonce[16], installkey[INSTALLKEYID], hash[HASHSIZE], sig[64];
        unsigned long long counter, recvus;
        vector<uchar> extras; // key-value

        signature() : version(VERSION), algorithm(ED25519), scope(FINAL), segment(0), counter(0), recvus(0)
        {
            memset(keyid, 0, sizeof(keyid)); memset(session, 0, sizeof(session)); memset(nonce, 0, sizeof(nonce));
            memset(installkey, 0, sizeof(installkey));
            memset(hash, 0, sizeof(hash)); memset(sig, 0, sizeof(sig));
        }
        // the signed bytes: "SBR2-sig" and every field before the signature
        void signedbytes(vector<uchar> &out) const;
        void sign(const uchar *seed); // fills keyid and sig from a 32-byte Ed25519 seed
        bool check(const uchar *publickey) const;
        void write(vector<uchar> &payload) const;
        bool read(const uchar *p, int len);
    };
    // a key's value in a key-value payload: svar/var/bool values in num, str values in str
    bool kvfind(const vector<uchar> &data, const char *key, long long *num, char *str = NULL, int strsize = 0);
    void keyid(const uchar *publickey, uchar *id);
    // the id a signature uses for a client's install key (its Noise static public key)
    void installkeyid(const uchar *publickey, uchar *id);
    void publickey(const uchar *seed, uchar *out);
}

#endif
