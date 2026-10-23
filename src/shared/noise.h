// Noise protocol (revision 34), Noise_XK_25519_ChaChaPoly_BLAKE2b, on Monocypher. A state
// machine only: handshake and transport messages go in and out as bytes, with no sockets.
//
//   XK:  <- s
//        ...
//        -> e, es
//        <- e, ee
//        -> s, se
//
// The initiator knows the responder's static key beforehand; the responder learns the
// initiator's static key from the third message.

#ifndef __NOISE_H__
#define __NOISE_H__

namespace noise
{
    enum { KEYLEN = 32, HASHLEN = 64, TAGLEN = 16, MAXMESSAGE = 65535 };

    // n bytes from the operating system's random number generator
    bool randombytes(uchar *out, int n);
    void publickey(const uchar *privatekey, uchar *publickey);

    struct cipherstate
    {
        uchar k[KEYLEN];
        bool haskey;
        unsigned long long n;

        cipherstate() : haskey(false), n(0) { memset(k, 0, sizeof(k)); }
        ~cipherstate() { wipe(); }
        void init(const uchar *key) { memcpy(k, key, KEYLEN); haskey = true; n = 0; }
        void wipe();
        // appends the ciphertext and tag (or the plaintext, without a key) to out
        bool encrypt(const uchar *ad, int adlen, const uchar *in, int len, vector<uchar> &out);
        // appends the plaintext to out; false if the tag does not match
        bool decrypt(const uchar *ad, int adlen, const uchar *in, int len, vector<uchar> &out);
    };

    struct handshake
    {
        enum { FAILED = -1 };

        bool initiator;
        int step; // messages done; 3 when the handshake is complete, FAILED after an error
        uchar ck[HASHLEN], h[HASHLEN];
        cipherstate cipher;
        uchar s[KEYLEN], spub[KEYLEN], e[KEYLEN], epub[KEYLEN], rs[KEYLEN], re[KEYLEN];
        bool fixedephemeral;

        // s: this side's static private key; remotestatic: the responder's static public key,
        // required for the initiator and ignored for the responder
        handshake(bool initiator, const uchar *prologue, int prologuelen, const uchar *s, const uchar *remotestatic);
        ~handshake();

        // uses priv as the next ephemeral key instead of a random one (test vectors only)
        void setephemeral(const uchar *priv);
        bool canwrite() const { return step >= 0 && step < 3 && (step % 2 == 0) == initiator; }
        bool canread() const { return step >= 0 && step < 3 && (step % 2 == 0) != initiator; }
        bool done() const { return step == 3; }

        // appends the next handshake message to out
        bool writemessage(const uchar *payload, int len, vector<uchar> &out);
        // appends the message's payload to payload; any failure ends the handshake
        bool readmessage(const uchar *msg, int len, vector<uchar> &payload);
        // after the last message: the keys for sending and receiving
        bool split(cipherstate &send, cipherstate &receive);
        const uchar *hash() const { return h; }
        const uchar *remotestatic() const { return rs; }

    private:
        void mixhash(const uchar *data, int len);
        void mixkey(const uchar *ikm, int len);
        bool dh(const uchar *priv, const uchar *pub);
        bool encryptandhash(const uchar *in, int len, vector<uchar> &out);
        bool decryptandhash(const uchar *in, int len, vector<uchar> &out);
        bool fail() { step = FAILED; return false; }
    };

    void hkdf(const uchar *chainkey, const uchar *ikm, int ikmlen, uchar *out1, uchar *out2);
}

#endif
