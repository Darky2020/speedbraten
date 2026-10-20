// Replay file building blocks: zstd, BLAKE3 and Ed25519 (RFC 8032).

#include "game.h"
#include "replayio.h"
#include "zstd.h"
#include "blake3.h"
#include "monocypher-ed25519.h"

namespace sbr2
{
    void tohex(const uchar *p, int n, char *out)
    {
        loopi(n) nformatstring(out + 2*i, 3, "%02x", p[i]);
    }

    static bool istype(const char *t, const char *want) { return !memcmp(t, want, 4); }
    static bool required(const char *t) { return t[0] >= 'A' && t[0] <= 'Z'; }

    static void hashlabel(blake3_hasher &h, const char *label) { blake3_hasher_update(&h, label, strlen(label)); }

    // hashed(c) = type || var(raw length) || raw payload
    static void hashchunk(blake3_hasher &h, const char *type, const uchar *data, int len)
    {
        vector<uchar> pre;
        byteswriter w(pre);
        w.bytes(type, 4);
        w.var(len);
        blake3_hasher_update(&h, pre.getbuf(), pre.length());
        if(len) blake3_hasher_update(&h, data, len);
    }

    static void starthead(uchar *head)
    {
        blake3_hasher h;
        blake3_hasher_init(&h);
        hashlabel(h, "SBR2-head");
        vector<uchar> v;
        byteswriter(v).var(VERSION);
        blake3_hasher_update(&h, v.getbuf(), v.length());
        blake3_hasher_finalize(&h, head, HASHSIZE);
    }

    static void advance(uchar *head, blake3_hasher &run, const char *label)
    {
        uchar digest[HASHSIZE];
        blake3_hasher_finalize(&run, digest, HASHSIZE);
        blake3_hasher h;
        blake3_hasher_init(&h);
        hashlabel(h, label);
        blake3_hasher_update(&h, head, HASHSIZE);
        blake3_hasher_update(&h, digest, HASHSIZE);
        blake3_hasher_finalize(&h, head, HASHSIZE);
        blake3_hasher_init(&run);
    }

    writer::writer() : segments(0), finished(false)
    {
        running = new blake3_hasher;
        blake3_hasher_init((blake3_hasher *)running);
        byteswriter w(out);
        w.bytes("SBR2", 4);
        w.var(VERSION);
        starthead(head);
        memset(content, 0, sizeof(content));
    }

    writer::~writer() { delete (blake3_hasher *)running; }

    void writer::add(const char *type, const vector<uchar> &payload, bool compress) { add(type, payload.getbuf(), payload.length(), compress); }

    void writer::add(const char *type, const uchar *payload, int len, bool compress)
    {
        if(finished && !istype(type, "sIGN")) return;
        if(!istype(type, "sIGN") && !istype(type, "END ")) hashchunk(*(blake3_hasher *)running, type, payload, len);
        vector<uchar> packed;
        int flags = 0;
        if(compress && len > 0)
        {
            packed.growbuf(int(ZSTD_compressBound(len)));
            size_t n = ZSTD_compress(packed.getbuf(), packed.capacity(), payload, len, 19);
            if(!ZSTD_isError(n) && int(n) < len) { packed.setsize(int(n)); flags |= FLAG_ZSTD; }
        }
        const uchar *stored = flags & FLAG_ZSTD ? packed.getbuf() : payload;
        int storedlen = flags & FLAG_ZSTD ? packed.length() : len;
        int start = out.length();
        byteswriter w(out);
        w.bytes(type, 4);
        w.var(flags);
        w.var(storedlen);
        if(storedlen) w.bytes(stored, storedlen);
        w.u32(uint(crc32(crc32(0, Z_NULL, 0), out.getbuf() + start, out.length() - start)));
    }

    const uchar *writer::endsegment(int endtick)
    {
        vector<uchar> p;
        byteswriter w(p);
        w.var(segments);
        w.var(endtick);
        add("SEGE", p);
        advance(head, *(blake3_hasher *)running, "SBR2-seg");
        segments++;
        return head;
    }

    const uchar *writer::finish()
    {
        if(finished) return content;
        memcpy(content, head, HASHSIZE);
        advance(content, *(blake3_hasher *)running, "SBR2-end");
        vector<uchar> p;
        byteswriter w(p);
        w.var(segments);
        w.bytes(content, HASHSIZE);
        add("END ", p);
        finished = true;
        return content;
    }

    vector<uchar> &kvwriter::add(const char *key, int type)
    {
        loopv(entries) if(!strcmp(entries[i]->key, key)) { entries[i]->type = type; entries[i]->value.setsize(0); return entries[i]->value; }
        entry *e = entries.add(new entry);
        copystring(e->key, key);
        e->type = type;
        return e->value;
    }

    static bool keyorder(const kvwriter::entry *a, const kvwriter::entry *b) { return strcmp(a->key, b->key) < 0; }

    void kvwriter::build(vector<uchar> &out)
    {
        entries.sort(keyorder);
        byteswriter w(out);
        w.var(entries.length());
        loopv(entries)
        {
            w.str(entries[i]->key);
            w.u8(entries[i]->type);
            w.bytes(entries[i]->value.getbuf(), entries[i]->value.length());
        }
    }

    const char KNOWNCHUNKS[] = "META" "DATA" "SETS" "ATTM" "INPS" "HASH" "SPLC" "RSLT" "SEGE" "END ";

    bool fromhex(const char *s, uchar *out, int n)
    {
        if(int(strlen(s)) != 2*n) return false;
        loopi(n)
        {
            if(!isxdigit(uchar(s[2*i])) || !isxdigit(uchar(s[2*i+1]))) return false;
            char b[3] = { s[2*i], s[2*i+1], '\0' };
            out[i] = uchar(strtoul(b, NULL, 16));
        }
        return true;
    }

    void appendchunk(vector<uchar> &out, const char *type, const vector<uchar> &payload)
    {
        int start = out.length();
        byteswriter w(out);
        w.bytes(type, 4);
        w.var(0);
        w.var(payload.length());
        w.bytes(payload.getbuf(), payload.length());
        w.u32(uint(crc32(crc32(0, Z_NULL, 0), out.getbuf() + start, out.length() - start)));
    }

    bool kvfind(const vector<uchar> &data, const char *key, long long *num, char *str, int strsize)
    {
        bytesreader r(data.getbuf(), data.length());
        int n = int(r.var());
        loopi(n)
        {
            string k;
            if(!r.str(k, sizeof(k))) return false;
            int type = r.u8();
            bool match = !strcmp(k, key);
            switch(type)
            {
                case VALUE_SVAR: { long long v = r.svar(); if(match && num) *num = v; break; }
                case VALUE_VAR: case VALUE_BOOL: { long long v = type == VALUE_VAR ? (long long)r.var() : r.u8(); if(match && num) *num = v; break; }
                case VALUE_STR: case VALUE_BYTES:
                {
                    unsigned long long len = r.var();
                    if(!r.ok || len > (unsigned long long)r.left()) return false;
                    if(match && str && strsize > 0) { int keep = min(int(len), strsize - 1); memcpy(str, r.p, keep); str[keep] = '\0'; }
                    r.p += len;
                    break;
                }
                case VALUE_F32: r.u32(); break;
                default: return false;
            }
            if(!r.ok) return false;
            if(match) return true;
        }
        return false;
    }

    void keyid(const uchar *publickey, uchar *id)
    {
        uchar h[HASHSIZE];
        blake3_hasher b;
        blake3_hasher_init(&b);
        blake3_hasher_update(&b, publickey, 32);
        blake3_hasher_finalize(&b, h, HASHSIZE);
        memcpy(id, h, 8);
    }

    void publickey(const uchar *seed, uchar *out)
    {
        uchar s[32], sk[64];
        memcpy(s, seed, 32);
        crypto_ed25519_key_pair(sk, out, s);
        memset(sk, 0, sizeof(sk));
    }

    static void writefields(const signature &s, vector<uchar> &out)
    {
        byteswriter w(out);
        w.u8(s.version);
        w.u8(s.algorithm);
        w.u8(s.scope);
        w.u8(0);
        w.bytes(s.keyid, 8);
        w.bytes(s.session, 16);
        w.bytes(s.nonce, 16);
        w.var(s.segment);
        w.var(s.counter);
        w.u64(s.recvus);
        w.bytes(s.hash, HASHSIZE);
        w.var(s.extras.length());
        w.bytes(s.extras.getbuf(), s.extras.length());
    }

    void signature::signedbytes(vector<uchar> &out) const
    {
        out.put((const uchar *)"SBR2-sig", 8);
        writefields(*this, out);
    }

    void signature::sign(const uchar *seed)
    {
        uchar s[32], sk[64], pk[32];
        memcpy(s, seed, 32);
        crypto_ed25519_key_pair(sk, pk, s);
        sbr2::keyid(pk, keyid);
        vector<uchar> msg;
        signedbytes(msg);
        crypto_ed25519_sign(sig, sk, msg.getbuf(), msg.length());
        memset(sk, 0, sizeof(sk));
    }

    bool signature::check(const uchar *publickey) const
    {
        uchar id[8];
        sbr2::keyid(publickey, id);
        if(memcmp(id, keyid, 8) || algorithm != ED25519) return false;
        vector<uchar> msg;
        signedbytes(msg);
        return !crypto_ed25519_check(sig, publickey, msg.getbuf(), msg.length());
    }

    void signature::write(vector<uchar> &payload) const
    {
        writefields(*this, payload);
        payload.put(sig, 64);
    }

    bool signature::read(const uchar *p, int len)
    {
        bytesreader r(p, len);
        version = r.u8();
        algorithm = r.u8();
        scope = r.u8();
        r.u8();
        r.bytes(keyid, 8);
        r.bytes(session, 16);
        r.bytes(nonce, 16);
        segment = int(r.var());
        counter = r.var();
        recvus = r.u64();
        r.bytes(hash, HASHSIZE);
        unsigned long long n = r.var();
        if(!r.ok || n > (unsigned long long)r.left()) return false;
        extras.setsize(0);
        extras.put(r.p, int(n));
        r.p += n;
        r.bytes(sig, 64);
        return r.ok && !r.left() && version == VERSION && scope <= FINAL;
    }

    const chunk *reader::find(const char *type, int from) const
    {
        for(int i = from; i < chunks.length(); i++) if(istype(chunks[i]->type, type)) return chunks[i];
        return NULL;
    }

    bool reader::parse(const uchar *buf, int len, const char *known)
    {
        chunks.deletecontents();
        heads.setsize(0);
        contentok = false;
        error[0] = '\0';
        bytesreader r(buf, len);
        char magic[4];
        if(!r.bytes(magic, 4) || memcmp(magic, "SBR2", 4)) { copystring(error, "not an SBR2 file"); return false; }
        unsigned long long version = r.var();
        if(!r.ok || version != VERSION) { formatstring(error, "unsupported container version %llu", version); return false; }
        uchar head[HASHSIZE];
        starthead(head);
        blake3_hasher run;
        blake3_hasher_init(&run);
        bool ended = false;
        int segment = 0;
        while(r.left() > 0)
        {
            const uchar *start = r.p;
            chunk c;
            c.type[4] = '\0';
            r.bytes(c.type, 4);
            c.flags = int(r.var());
            unsigned long long stored = r.var();
            if(!r.ok) { formatstring(error, "chunk %d: malformed header", chunks.length()); return false; }
            if(stored > (unsigned long long)r.left()) { formatstring(error, "chunk %d is cut off", chunks.length()); return false; }
            const uchar *payload = r.p;
            r.p += stored;
            uint crc = r.u32();
            if(!r.ok) { formatstring(error, "chunk %d (%s) is cut off", chunks.length(), c.type); return false; }
            if(crc != uint(crc32(crc32(0, Z_NULL, 0), start, int(r.p - 4 - start)))) { formatstring(error, "chunk %d (%s): CRC mismatch", chunks.length(), c.type); return false; }
            if(c.flags & ~FLAG_ZSTD) { formatstring(error, "chunk %d (%s): unknown flags %d", chunks.length(), c.type, c.flags); return false; }
            if(c.flags & FLAG_ZSTD)
            {
                unsigned long long raw = ZSTD_getFrameContentSize(payload, size_t(stored));
                if(raw == ZSTD_CONTENTSIZE_ERROR || raw == ZSTD_CONTENTSIZE_UNKNOWN || raw > (1ULL<<30)) { formatstring(error, "chunk %d (%s): bad compressed payload", chunks.length(), c.type); return false; }
                c.data.growbuf(int(raw));
                size_t n = ZSTD_decompress(c.data.getbuf(), size_t(raw), payload, size_t(stored));
                if(ZSTD_isError(n) || n != raw) { formatstring(error, "chunk %d (%s): bad compressed payload", chunks.length(), c.type); return false; }
                c.data.setsize(int(n));
            }
            else c.data.put(payload, int(stored));
            bool sign = istype(c.type, "sIGN"), end = istype(c.type, "END ");
            if(ended && !sign) { formatstring(error, "chunk %d (%s) after END", chunks.length(), c.type); return false; }
            if(required(c.type) && !strstr(known, c.type)) { formatstring(error, "unknown required chunk %s", c.type); return false; }
            c.segment = segment;
            if(!sign && !end) hashchunk(run, c.type, c.data.getbuf(), c.data.length());
            if(istype(c.type, "SEGE"))
            {
                bytesreader s(c.data.getbuf(), c.data.length());
                unsigned long long index = s.var();
                s.var();
                if(!s.ok || s.left() || index != (unsigned long long)segment) { formatstring(error, "segment %d: bad SEGE", segment); return false; }
                advance(head, run, "SBR2-seg");
                heads.put(head, HASHSIZE);
                segment++;
            }
            if(end)
            {
                memcpy(content, head, HASHSIZE);
                advance(content, run, "SBR2-end");
                bytesreader s(c.data.getbuf(), c.data.length());
                unsigned long long count = s.var();
                uchar stored[HASHSIZE];
                s.bytes(stored, HASHSIZE);
                if(!s.ok || s.left() || count != (unsigned long long)segment) { copystring(error, "bad END chunk"); return false; }
                contentok = !memcmp(stored, content, HASHSIZE);
                if(!contentok) { copystring(error, "content hash mismatch"); return false; }
                ended = true;
            }
            chunk *keep = new chunk;
            memcpy(keep->type, c.type, 5);
            keep->flags = c.flags;
            keep->segment = c.segment;
            keep->data.move(c.data);
            chunks.add(keep);
        }
        if(!ended) { copystring(error, "no END chunk"); return false; }
        return true;
    }
}

namespace game
{
    using sbr2::tohex;

    static void fromhex(const char *s, uchar *out)
    {
        for(int i = 0; s[2*i] && s[2*i+1]; i++)
        {
            char b[3] = { s[2*i], s[2*i+1], '\0' };
            out[i] = uchar(strtoul(b, NULL, 16));
        }
    }

    // known-answer checks of the libraries as built into this binary; returns the number of failures
    static int selftest()
    {
        int failures = 0;
        uchar out[64];
        char hex[129];

        blake3_hasher b;
        blake3_hasher_init(&b);
        blake3_hasher_finalize(&b, out, 32);
        tohex(out, 32, hex);
        if(strcmp(hex, "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262")) { conoutf(CON_ERROR, "selftest: blake3 %s", hex); failures++; }

        // RFC 8032 section 7.1, test 1
        uchar seed[32], sk[64], pk[32], sig[64];
        fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed);
        crypto_ed25519_key_pair(sk, pk, seed);
        tohex(pk, 32, hex);
        if(strcmp(hex, "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")) { conoutf(CON_ERROR, "selftest: ed25519 key %s", hex); failures++; }
        crypto_ed25519_sign(sig, sk, NULL, 0);
        tohex(sig, 64, hex);
        if(strcmp(hex, "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")) { conoutf(CON_ERROR, "selftest: ed25519 signature"); failures++; }
        if(crypto_ed25519_check(sig, pk, NULL, 0)) { conoutf(CON_ERROR, "selftest: ed25519 check"); failures++; }
        sig[0] ^= 1;
        if(!crypto_ed25519_check(sig, pk, NULL, 0)) { conoutf(CON_ERROR, "selftest: ed25519 accepted a bad signature"); failures++; }

        vector<char> src, packed, unpacked;
        loopi(100000) src.add("speedbraten replay "[i % 19]);
        packed.growbuf(int(ZSTD_compressBound(src.length())));
        size_t n = ZSTD_compress(packed.getbuf(), packed.capacity(), src.getbuf(), src.length(), 19);
        unpacked.growbuf(src.length());
        size_t m = ZSTD_isError(n) ? 0 : ZSTD_decompress(unpacked.getbuf(), unpacked.capacity(), packed.getbuf(), n);
        if(ZSTD_isError(n) || ZSTD_isError(m) || m != size_t(src.length()) || memcmp(src.getbuf(), unpacked.getbuf(), m)) { conoutf(CON_ERROR, "selftest: zstd round trip"); failures++; }
        return failures;
    }

    ICOMMAND(replayselftest, "", (),
    {
        int failures = selftest();
        conoutf("replay selftest: %s (blake3, ed25519, zstd %s)", failures ? "FAILED" : "ok", ZSTD_versionString());
        intret(failures);
    });
}

namespace game
{
    // harness: a sample SBR2 file with every container feature, for checking readers
    ICOMMAND(sbr2writetest, "s", (char *file),
    {
        sbr2::writer w;
        vector<uchar> p;
        sbr2::byteswriter b(p);
        b.var(2); b.str("client"); b.u8(2); b.str("speedbraten"); b.str("player"); b.u8(2); b.str("tester");
        w.add("META", p);
        p.setsize(0); b.str("an optional chunk this reader does not know");
        w.add("xTRA", p);
        loopk(3)
        {
            p.setsize(0);
            loopi(2000) b.u8(i % 7 + k);
            w.add("INPS", p, true);
            p.setsize(0); b.var(k*200); b.u32(0xdeadbeef ^ k);
            w.add("HASH", p);
            w.endsegment((k+1)*200);
        }
        w.finish();
        stream *f = openrawfile(path(file, true), "wb");
        if(!f) { conoutf(CON_ERROR, "sbr2writetest: cannot write %s", file); return; }
        f->write(w.out.getbuf(), w.out.length());
        delete f;
        string hex;
        sbr2::tohex(w.content, sbr2::HASHSIZE, hex);
        conoutf("sbr2writetest: %s, %d bytes, content %s", file, w.out.length(), hex);
    });

    // sbr2check file: verifies an SBR2 file and prints its chunks, segment heads and content hash
    static void replaytestkey(const char *seedhex)
    {
        uchar seed[32], pk[32];
        if(!sbr2::fromhex(seedhex, seed, 32)) { conoutf(CON_ERROR, "replaytestkey: the seed is 64 hex digits"); return; }
        sbr2::publickey(seed, pk);
        string hex;
        tohex(pk, 32, hex);
        conoutf("replaytestkey: %s", hex);
        result(hex);
    }
    COMMAND(replaytestkey, "s");

    // Until a witness exists: appends a signature made with the key of this seed, over the
    // content hash (scope 1) or the chain head of a segment (scope 0).
    static void replaysign(const char *file, const char *seedhex, int scope, int segment)
    {
        uchar seed[32];
        if(!sbr2::fromhex(seedhex, seed, 32)) { conoutf(CON_ERROR, "replaysign: the seed is 64 hex digits"); return; }
        stream *f = openrawfile(path(file, true), "rb");
        if(!f) { conoutf(CON_ERROR, "replaysign: could not open %s", file); return; }
        vector<uchar> buf;
        uchar chunk[65536];
        for(size_t n; (n = f->read(chunk, sizeof(chunk))) > 0;) buf.put(chunk, int(n));
        delete f;
        sbr2::reader rd;
        if(!rd.parse(buf.getbuf(), buf.length(), sbr2::KNOWNCHUNKS)) { conoutf(CON_ERROR, "replaysign: %s", rd.error); return; }
        sbr2::signature s;
        s.scope = scope ? int(sbr2::signature::FINAL) : int(sbr2::signature::SEGMENT);
        if(s.scope == sbr2::signature::SEGMENT)
        {
            if(segment < 0 || segment >= rd.heads.length() / sbr2::HASHSIZE) { conoutf(CON_ERROR, "replaysign: no segment %d", segment); return; }
            s.segment = segment;
            memcpy(s.hash, &rd.heads[segment * sbr2::HASHSIZE], sbr2::HASHSIZE);
        }
        else memcpy(s.hash, rd.content, sbr2::HASHSIZE);
        s.recvus = (unsigned long long)time(NULL) * 1000000;
        s.sign(seed);
        vector<uchar> payload;
        s.write(payload);
        sbr2::appendchunk(buf, "sIGN", payload);
        f = openrawfile(path(file, true), "wb");
        if(!f) { conoutf(CON_ERROR, "replaysign: could not write %s", file); return; }
        f->write(buf.getbuf(), buf.length());
        delete f;
        conoutf("replaysign: signed %s (%s)", file, s.scope ? "content" : "segment");
    }
    ICOMMAND(replaysign, "ssii", (char *file, char *seed, int *scope, int *segment), replaysign(file, seed, *scope, *segment));

    ICOMMAND(sbr2check, "s", (char *file),
    {
        stream *f = openrawfile(path(file, true), "rb");
        if(!f) { conoutf(CON_ERROR, "sbr2check: cannot read %s", file); intret(0); return; }
        vector<uchar> buf;
        buf.growbuf(int(f->size()));
        buf.advance(int(f->read(buf.getbuf(), int(f->size()))));
        delete f;
        sbr2::reader r;
        bool ok = r.parse(buf.getbuf(), buf.length(), "META" "INPS" "HASH" "SEGE" "END ");
        if(!ok) { conoutf(CON_ERROR, "sbr2check: %s: %s", file, r.error); intret(0); return; }
        loopv(r.chunks) conoutf("sbr2check: chunk %d %s segment %d flags %d %d bytes", i, r.chunks[i]->type, r.chunks[i]->segment, r.chunks[i]->flags, r.chunks[i]->data.length());
        loopi(r.heads.length()/sbr2::HASHSIZE)
        {
            string hex;
            sbr2::tohex(&r.heads[i*sbr2::HASHSIZE], sbr2::HASHSIZE, hex);
            conoutf("sbr2check: head %d %s", i, hex);
        }
        string hex;
        sbr2::tohex(r.content, sbr2::HASHSIZE, hex);
        conoutf("sbr2check: ok, content %s", hex);
        intret(1);
    });
}
