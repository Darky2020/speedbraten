#include "cube.h"
#include "noise.h"
#include "monocypher.h"

#ifdef WIN32
#include <bcrypt.h>
#endif

namespace noise
{
    static const char PROTOCOL[] = "Noise_XK_25519_ChaChaPoly_BLAKE2b";
    enum { BLOCKLEN = 128 };

    bool randombytes(uchar *out, int n)
    {
#ifdef WIN32
        return BCryptGenRandom(NULL, out, ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
        FILE *f = fopen("/dev/urandom", "rb");
        if(!f) return false;
        bool ok = fread(out, 1, n, f) == size_t(n);
        fclose(f);
        return ok;
#endif
    }

    void publickey(const uchar *privatekey, uchar *publickey) { crypto_x25519_public_key(publickey, privatekey); }

    static void hmac(const uchar *key, const uchar *data1, int len1, const uchar *data2, int len2, uchar *out)
    {
        uchar block[BLOCKLEN], inner[HASHLEN];
        memset(block, 0, sizeof(block));
        memcpy(block, key, HASHLEN);
        crypto_blake2b_ctx c;
        loopi(BLOCKLEN) block[i] ^= 0x36;
        crypto_blake2b_init(&c, HASHLEN);
        crypto_blake2b_update(&c, block, BLOCKLEN);
        crypto_blake2b_update(&c, data1, len1);
        if(len2) crypto_blake2b_update(&c, data2, len2);
        crypto_blake2b_final(&c, inner);
        loopi(BLOCKLEN) block[i] ^= 0x36 ^ 0x5c;
        crypto_blake2b_init(&c, HASHLEN);
        crypto_blake2b_update(&c, block, BLOCKLEN);
        crypto_blake2b_update(&c, inner, HASHLEN);
        crypto_blake2b_final(&c, out);
        crypto_wipe(block, sizeof(block));
        crypto_wipe(inner, sizeof(inner));
    }

    void hkdf(const uchar *chainkey, const uchar *ikm, int ikmlen, uchar *out1, uchar *out2)
    {
        uchar temp[HASHLEN], one = 1, two = 2;
        hmac(chainkey, ikm, ikmlen, NULL, 0, temp);
        hmac(temp, &one, 1, NULL, 0, out1);
        hmac(temp, out1, HASHLEN, &two, 1, out2);
        crypto_wipe(temp, sizeof(temp));
    }

    static void nonce(unsigned long long n, uchar *out)
    {
        memset(out, 0, 4);
        loopi(8) out[4 + i] = uchar(n >> (8*i));
    }

    void cipherstate::wipe()
    {
        crypto_wipe(k, sizeof(k));
        haskey = false;
        n = 0;
    }

    bool cipherstate::encrypt(const uchar *ad, int adlen, const uchar *in, int len, vector<uchar> &out)
    {
        if(!haskey) { out.put(in, len); return true; }
        if(n == ~0ULL) return false;
        uchar iv[12];
        nonce(n++, iv);
        int start = out.length();
        out.pad(len + TAGLEN);
        crypto_aead_ctx c;
        crypto_aead_init_ietf(&c, k, iv);
        crypto_aead_write(&c, out.getbuf() + start, out.getbuf() + start + len, ad, adlen, in, len);
        crypto_wipe(&c, sizeof(c));
        return true;
    }

    bool cipherstate::decrypt(const uchar *ad, int adlen, const uchar *in, int len, vector<uchar> &out)
    {
        if(!haskey) { out.put(in, len); return true; }
        if(len < TAGLEN || n == ~0ULL) return false;
        uchar iv[12];
        nonce(n, iv);
        int start = out.length(), plain = len - TAGLEN;
        out.pad(plain);
        crypto_aead_ctx c;
        crypto_aead_init_ietf(&c, k, iv);
        bool ok = !crypto_aead_read(&c, out.getbuf() + start, in + plain, ad, adlen, in, plain);
        crypto_wipe(&c, sizeof(c));
        if(!ok) { out.setsize(start); return false; }
        n++;
        return true;
    }

    handshake::handshake(bool initiator, const uchar *prologue, int prologuelen, const uchar *spriv, const uchar *remotestatic)
        : initiator(initiator), step(0), fixedephemeral(false)
    {
        memset(h, 0, sizeof(h));
        memcpy(h, PROTOCOL, strlen(PROTOCOL));
        memcpy(ck, h, HASHLEN);
        memcpy(s, spriv, KEYLEN);
        publickey(s, spub);
        memset(e, 0, sizeof(e)); memset(epub, 0, sizeof(epub)); memset(re, 0, sizeof(re));
        if(initiator) memcpy(rs, remotestatic, KEYLEN);
        else memset(rs, 0, sizeof(rs));
        mixhash(prologue, prologuelen);
        mixhash(initiator ? rs : spub, KEYLEN);
    }

    handshake::~handshake()
    {
        crypto_wipe(s, sizeof(s));
        crypto_wipe(e, sizeof(e));
        crypto_wipe(ck, sizeof(ck));
    }

    void handshake::setephemeral(const uchar *priv)
    {
        memcpy(e, priv, KEYLEN);
        fixedephemeral = true;
    }

    void handshake::mixhash(const uchar *data, int len)
    {
        crypto_blake2b_ctx c;
        crypto_blake2b_init(&c, HASHLEN);
        crypto_blake2b_update(&c, h, HASHLEN);
        crypto_blake2b_update(&c, data, len);
        crypto_blake2b_final(&c, h);
    }

    void handshake::mixkey(const uchar *ikm, int len)
    {
        uchar k[HASHLEN];
        hkdf(ck, ikm, len, ck, k);
        cipher.init(k);
        crypto_wipe(k, sizeof(k));
    }

    // an all-zero result means a low-order public key
    bool handshake::dh(const uchar *priv, const uchar *pub)
    {
        uchar shared[KEYLEN], zero[KEYLEN];
        crypto_x25519(shared, priv, pub);
        memset(zero, 0, sizeof(zero));
        bool ok = crypto_verify32(shared, zero) != 0;
        if(ok) mixkey(shared, KEYLEN);
        crypto_wipe(shared, sizeof(shared));
        return ok;
    }

    bool handshake::encryptandhash(const uchar *in, int len, vector<uchar> &out)
    {
        int start = out.length();
        if(!cipher.encrypt(h, HASHLEN, in, len, out)) return false;
        mixhash(out.getbuf() + start, out.length() - start);
        return true;
    }

    bool handshake::decryptandhash(const uchar *in, int len, vector<uchar> &out)
    {
        if(!cipher.decrypt(h, HASHLEN, in, len, out)) return false;
        mixhash(in, len);
        return true;
    }

    bool handshake::writemessage(const uchar *payload, int len, vector<uchar> &out)
    {
        if(!canwrite()) return fail();
        int start = out.length();
        if(step < 2)
        {
            if(!fixedephemeral && !randombytes(e, KEYLEN)) return fail();
            fixedephemeral = false;
            publickey(e, epub);
            out.put(epub, KEYLEN);
            mixhash(epub, KEYLEN);
            if(!dh(e, step == 0 ? rs : re)) return fail(); // es, ee
        }
        else
        {
            if(!encryptandhash(spub, KEYLEN, out)) return fail();
            if(!dh(s, re)) return fail(); // se
        }
        if(!encryptandhash(payload, len, out) || out.length() - start > MAXMESSAGE) return fail();
        step++;
        return true;
    }

    bool handshake::readmessage(const uchar *msg, int len, vector<uchar> &payload)
    {
        if(!canread() || len > MAXMESSAGE) return fail();
        const uchar *p = msg, *end = msg + len;
        if(step < 2)
        {
            if(end - p < KEYLEN) return fail();
            memcpy(re, p, KEYLEN);
            p += KEYLEN;
            mixhash(re, KEYLEN);
            if(!dh(step == 0 ? s : e, re)) return fail(); // es, ee
        }
        else
        {
            if(end - p < KEYLEN + TAGLEN) return fail();
            vector<uchar> key;
            if(!decryptandhash(p, KEYLEN + TAGLEN, key)) return fail();
            memcpy(rs, key.getbuf(), KEYLEN);
            p += KEYLEN + TAGLEN;
            if(!dh(e, rs)) return fail(); // se
        }
        int start = payload.length();
        if(!decryptandhash(p, int(end - p), payload)) { payload.setsize(start); return fail(); }
        step++;
        return true;
    }

    bool handshake::split(cipherstate &send, cipherstate &receive)
    {
        if(!done()) return false;
        uchar k1[HASHLEN], k2[HASHLEN];
        hkdf(ck, NULL, 0, k1, k2);
        send.init(initiator ? k1 : k2);
        receive.init(initiator ? k2 : k1);
        crypto_wipe(k1, sizeof(k1));
        crypto_wipe(k2, sizeof(k2));
        return true;
    }
}
