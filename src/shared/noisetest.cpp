// noiseselftest: the Noise test vectors for Noise_XK_25519_ChaChaPoly_BLAKE2b, then checks that
// tampered, replayed and truncated messages and a wrong responder key are refused.

#include "cube.h"
#include "noise.h"
#include "noisevectors.h"

namespace noise
{
    static void unhex(const char *s, vector<uchar> &out)
    {
        out.setsize(0);
        for(; s[0] && s[1]; s += 2)
        {
            char b[3] = { s[0], s[1], '\0' };
            out.add(uchar(strtoul(b, NULL, 16)));
        }
    }

    static bool same(const vector<uchar> &a, const vector<uchar> &b) { return a.length() == b.length() && !memcmp(a.getbuf(), b.getbuf(), a.length()); }

    static int runvector(int index, const noisevector &v)
    {
        vector<uchar> iprologue, rprologue, istatic, iephemeral, irs, rstatic, rephemeral, hh;
        unhex(v.init_prologue, iprologue); unhex(v.resp_prologue, rprologue);
        unhex(v.init_static, istatic); unhex(v.init_ephemeral, iephemeral); unhex(v.init_remote_static, irs);
        unhex(v.resp_static, rstatic); unhex(v.resp_ephemeral, rephemeral); unhex(v.handshake_hash, hh);
        handshake ini(true, iprologue.getbuf(), iprologue.length(), istatic.getbuf(), irs.getbuf());
        handshake res(false, rprologue.getbuf(), rprologue.length(), rstatic.getbuf(), NULL);
        ini.setephemeral(iephemeral.getbuf());
        res.setephemeral(rephemeral.getbuf());
        cipherstate isend, irecv, rsend, rrecv;
        int failures = 0;
        loopi(v.messages)
        {
            vector<uchar> payload, want, msg, got;
            unhex(v.payload[i], payload);
            unhex(v.ciphertext[i], want);
            bool fromini = i % 2 == 0, ok;
            if(i < 3)
            {
                handshake &w = fromini ? ini : res, &r = fromini ? res : ini;
                ok = w.writemessage(payload.getbuf(), payload.length(), msg) && same(msg, want) &&
                     r.readmessage(msg.getbuf(), msg.length(), got) && same(got, payload);
                if(ok && i == 2)
                {
                    ok = ini.split(isend, irecv) && res.split(rsend, rrecv) && !memcmp(ini.hash(), res.hash(), HASHLEN) &&
                         (hh.empty() || !memcmp(ini.hash(), hh.getbuf(), HASHLEN)) &&
                         !memcmp(res.remotestatic(), ini.spub, KEYLEN);
                }
            }
            else
            {
                cipherstate &w = fromini ? isend : rsend, &r = fromini ? rrecv : irecv;
                ok = w.encrypt(NULL, 0, payload.getbuf(), payload.length(), msg) && same(msg, want) &&
                     r.decrypt(NULL, 0, msg.getbuf(), msg.length(), got) && same(got, payload);
            }
            if(!ok) { conoutf(CON_ERROR, "noiseselftest: vector %d message %d", index, i); failures++; break; }
        }
        return failures;
    }

    // a full handshake between fresh keys; returns false if any step fails
    static bool connect(handshake &ini, handshake &res, cipherstate &isend, cipherstate &irecv, cipherstate &rsend, cipherstate &rrecv)
    {
        vector<uchar> m1, m2, m3, p;
        return ini.writemessage(NULL, 0, m1) && res.readmessage(m1.getbuf(), m1.length(), p) &&
               res.writemessage(NULL, 0, m2) && ini.readmessage(m2.getbuf(), m2.length(), p) &&
               ini.writemessage(NULL, 0, m3) && res.readmessage(m3.getbuf(), m3.length(), p) &&
               ini.split(isend, irecv) && res.split(rsend, rrecv);
    }

    static int refusals()
    {
        int failures = 0;
        uchar is[KEYLEN], rs[KEYLEN], rpub[KEYLEN], other[KEYLEN], otherpub[KEYLEN];
        const uchar prologue[] = "speedbraten/1 test";
        if(!randombytes(is, KEYLEN) || !randombytes(rs, KEYLEN) || !randombytes(other, KEYLEN)) { conoutf(CON_ERROR, "noiseselftest: no random bytes"); return 1; }
        publickey(rs, rpub);
        publickey(other, otherpub);

        {
            handshake ini(true, prologue, sizeof(prologue), is, otherpub), res(false, prologue, sizeof(prologue), rs, NULL);
            vector<uchar> m1, p;
            if(!ini.writemessage(NULL, 0, m1) || res.readmessage(m1.getbuf(), m1.length(), p)) { conoutf(CON_ERROR, "noiseselftest: a wrong responder key was accepted"); failures++; }
        }
        {
            handshake ini(true, prologue, sizeof(prologue), is, rpub), res(false, prologue, sizeof(prologue) - 1, rs, NULL);
            vector<uchar> m1, p;
            if(!ini.writemessage(NULL, 0, m1) || res.readmessage(m1.getbuf(), m1.length(), p)) { conoutf(CON_ERROR, "noiseselftest: a different prologue was accepted"); failures++; }
        }
        {
            handshake ini(true, prologue, sizeof(prologue), is, rpub), res(false, prologue, sizeof(prologue), rs, NULL);
            cipherstate isend, irecv, rsend, rrecv;
            if(!connect(ini, res, isend, irecv, rsend, rrecv)) { conoutf(CON_ERROR, "noiseselftest: handshake failed"); return failures + 1; }
            const uchar text[] = "transport message";
            vector<uchar> a, b, p;
            isend.encrypt(NULL, 0, text, sizeof(text), a);
            isend.encrypt(NULL, 0, text, sizeof(text), b);

            vector<uchar> flipped(a);
            flipped[3] ^= 1;
            if(rrecv.decrypt(NULL, 0, flipped.getbuf(), flipped.length(), p)) { conoutf(CON_ERROR, "noiseselftest: a flipped byte was accepted"); failures++; }
            if(rrecv.decrypt(NULL, 0, a.getbuf(), a.length() - 1, p)) { conoutf(CON_ERROR, "noiseselftest: a truncated message was accepted"); failures++; }
            if(rrecv.decrypt(NULL, 0, b.getbuf(), b.length(), p)) { conoutf(CON_ERROR, "noiseselftest: a reordered message was accepted"); failures++; }
            p.setsize(0);
            if(!rrecv.decrypt(NULL, 0, a.getbuf(), a.length(), p) || p.length() != int(sizeof(text))) { conoutf(CON_ERROR, "noiseselftest: a good message was refused"); failures++; }
            if(rrecv.decrypt(NULL, 0, a.getbuf(), a.length(), p)) { conoutf(CON_ERROR, "noiseselftest: a replayed message was accepted"); failures++; }
        }
        return failures;
    }

    int selftest()
    {
        int failures = 0;
        for(size_t i = 0; i < sizeof(noisevectors)/sizeof(noisevectors[0]); i++) failures += runvector(int(i), noisevectors[i]);
        failures += refusals();
        return failures;
    }

    ICOMMAND(noiseselftest, "", (),
    {
        int failures = selftest();
        conoutf("noise selftest: %s (%d vectors, Noise_XK_25519_ChaChaPoly_BLAKE2b)", failures ? "FAILED" : "ok", int(sizeof(noisevectors)/sizeof(noisevectors[0])));
        intret(failures);
    });
}
