// Replay file building blocks: zstd, BLAKE3 and Ed25519 (RFC 8032).

#include "game.h"
#include "zstd.h"
#include "blake3.h"
#include "monocypher-ed25519.h"

namespace game
{
    static void tohex(const uchar *p, int n, char *out)
    {
        loopi(n) nformatstring(out + 2*i, 3, "%02x", p[i]);
    }

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
