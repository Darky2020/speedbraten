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
