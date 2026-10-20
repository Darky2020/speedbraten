// Run-level metadata for replay files: the client that played the run, the session around it,
// the player's history before it, how it was produced, the input settings and the window. None
// of it affects playback; it lets a verifier see runs that don't look like ordinary live play.

#include "game.h"
#include "inputs.h"
#include "replayio.h"
#include "blake3.h"
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef SPEEDBRATEN_COMMIT
#define SPEEDBRATEN_COMMIT "unknown"
#endif
#ifndef SPEEDBRATEN_DIRTY
#define SPEEDBRATEN_DIRTY 0
#endif

#if defined(WIN32)
#define TELLS_OS "windows"
#elif defined(__APPLE__)
#define TELLS_OS "macos"
#else
#define TELLS_OS "linux"
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#define TELLS_ARCH "arm64"
#else
#define TELLS_ARCH "x86_64"
#endif

extern int screenw, screenh;

namespace game
{
    static const time_t sessionstart = time(NULL);
    static int runssession = 0, resets = 0;

    // counters since the current run began
    static struct
    {
        Uint64 startcounter;
        uint startms;
        int saves, loads, focuslosses, minimizes, fixedframe;
        long long focuslostus;
        bool harness, dethash, edit;
    } run;
    static bool lastfocus = true, lastminimized = false;

    void tellsrunstart()
    {
        runssession++;
        memset(&run, 0, sizeof(run));
        run.startms = SDL_GetTicks();
        run.startcounter = SDL_GetPerformanceCounter();
        lastfocus = hasfocus;
        lastminimized = minimized;
    }

    void tellssave() { run.saves++; }
    void tellsload() { run.loads++; }
    void tellsreset() { resets++; }
    void tellsharness() { run.harness = true; }
    int tellsrunms() { return int(SDL_GetTicks() - run.startms); }
    long long tellsrunus() { return (long long)((SDL_GetPerformanceCounter() - run.startcounter)*1000000/SDL_GetPerformanceFrequency()); }

    void tellsframe(int us)
    {
        if(lastfocus && !hasfocus) run.focuslosses++;
        if(!lastminimized && minimized) run.minimizes++;
        if(!hasfocus) run.focuslostus += us;
        lastfocus = hasfocus;
        lastminimized = minimized;
        if(editmode) run.edit = true;
        if(dethashactive()) run.dethash = true;
        run.fixedframe = max(run.fixedframe, fixedframe);
    }

    static bool hashpath(const char *file, uchar *out)
    {
        FILE *f = fopen(file, "rb");
        if(!f) return false;
        blake3_hasher h;
        blake3_hasher_init(&h);
        uchar buf[65536];
        for(size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) blake3_hasher_update(&h, buf, n);
        fclose(f);
        blake3_hasher_finalize(&h, out, 32);
        return true;
    }

    static bool exehash(uchar *out)
    {
        static int state = 0; // 1 hashed, 2 failed
        static uchar hash[32];
        if(!state)
        {
            char file[4096] = "";
#if defined(WIN32)
            if(!GetModuleFileNameA(NULL, file, sizeof(file))) file[0] = '\0';
#elif defined(__APPLE__)
            uint32_t size = sizeof(file);
            if(_NSGetExecutablePath(file, &size)) file[0] = '\0';
#else
            copystring(file, "/proc/self/exe", sizeof(file));
#endif
            state = file[0] && hashpath(file, hash) ? 1 : 2;
        }
        if(state != 1) return false;
        memcpy(out, hash, 32);
        return true;
    }

    static void addchunk(vector<runchunk *> &out, const char *type, sbr2::kvwriter &kv)
    {
        runchunk *c = out.add(new runchunk);
        memcpy(c->type, type, sizeof(c->type));
        kv.build(c->data);
    }

    static void addsetting(sbr2::kvwriter &kv, const char *name)
    {
        ident *id = getident(name);
        if(!id) return;
        if(id->type == ID_VAR) kv.svar(name, *id->storage.i);
        else if(id->type == ID_FVAR) kv.f32(name, *id->storage.f);
    }

    static int bucket(long long v, const int *limits, int n)
    {
        int b = 0;
        while(b < n && v >= limits[b]) b++;
        return b;
    }

    static void histogram(sbr2::kvwriter &kv, const char *key, const int *counts, int n)
    {
        string s = "";
        loopi(n) concformatstring(s, "%s%d", i ? " " : "", counts[i]);
        kv.str(key, s);
    }

    // key presses and holds, mouse motion sizes and gaps over the whole run
    static void rawstatistics(const inputrun &r, sbr2::kvwriter &kv)
    {
        static const int holdlimits[7] = { 20000, 40000, 60000, 80000, 120000, 200000, 500000 }; // us
        static const int sizelimits[7] = { 2, 4, 8, 16, 32, 64, 128 };                           // |dx|+|dy|
        int holds[8] = { 0 }, sizes[8] = { 0 }, presses = 0, motions = 0;
        long long minhold = -1, mingap = -1;
        loopvk(r.attempts)
        {
            const vector<rawevent> &raw = r.attempts[k]->raw;
            long long lastmotion = -1;
            loopv(raw)
            {
                const rawevent &e = raw[i];
                if(e.kind == rawevent::MOTION)
                {
                    motions++;
                    sizes[bucket(abs(e.dx) + abs(e.dy), sizelimits, 7)]++;
                    if(lastmotion >= 0 && e.us > lastmotion && (mingap < 0 || e.us - lastmotion < mingap)) mingap = e.us - lastmotion;
                    lastmotion = e.us;
                }
                else if((e.kind == rawevent::KEYDOWN && !(e.flags & rawevent::REPEAT)) || e.kind == rawevent::BUTTONDOWN)
                {
                    presses++;
                    int up = e.kind == rawevent::KEYDOWN ? int(rawevent::KEYUP) : int(rawevent::BUTTONUP);
                    for(int j = i+1; j < raw.length(); j++) if(raw[j].kind == up && raw[j].code == e.code)
                    {
                        long long hold = raw[j].us - e.us;
                        holds[bucket(hold, holdlimits, 7)]++;
                        if(minhold < 0 || hold < minhold) minhold = hold;
                        break;
                    }
                }
            }
        }
        kv.var("presses", presses);
        kv.svar("minholdus", minhold);
        histogram(kv, "holdhist", holds, 8);
        kv.var("motions", motions);
        kv.svar("mingapus", mingap);
        histogram(kv, "motionhist", sizes, 8);
    }

    static const char * const inputsettings[] = { "sensitivity", "sensitivityscale", "mouseaccel", "invmouse", "zoomsens", "zoomaccel", "zoomautosens", "fov" };

    void tellsencode(const inputrun &r, const replayinfo &info, vector<runchunk *> &out)
    {
        uchar hash[32];
        sbr2::kvwriter client;
        ident *version = getident("p1xbratenversion");
        client.str("build", version && version->type == ID_SVAR ? *version->storage.s : "");
        client.str("commit", SPEEDBRATEN_COMMIT);
        client.boolean("dirty", SPEEDBRATEN_DIRTY != 0);
        client.str("compiler", __VERSION__);
        client.str("platform", TELLS_OS "-" TELLS_ARCH);
        client.var("simversion", info.simversion);
        if(exehash(hash)) client.bytes("exehash", hash, 32);
        const char *autoexec = findfile("autoexec.cfg", "rb");
        if(fileexists(autoexec, "r") && hashpath(autoexec, hash)) client.bytes("autoexec", hash, 32);
        addchunk(out, "cLNT", client);

        sbr2::kvwriter session;
        session.var("sessionstart", (unsigned long long)sessionstart);
        session.var("sessionms", SDL_GetTicks());
        session.var("runssession", runssession);
        session.var("resets", resets);
        session.var("saves", run.saves);
        session.var("loads", run.loads);
        session.var("runms", tellsrunms());
        addchunk(out, "sESS", session);

        sbr2::kvwriter history;
        vector<splittime> pb;
        vector<splitsegment> best;
        int pbms = -1;
        statssplitcompare(info.map, info.race ? "race" : info.flagrun ? "flagrun" : "sp", info.mode, info.splices > 0, pb, best, pbms);
        history.svar("pbms", pbms);
        long long bestsum = best.length() ? 0 : -1;
        loopv(best) bestsum += best[i].ticks * SIMTICK;
        history.svar("bestsum", bestsum);
        int count = 0, attempts = 0;
        if(statshistory(hash, count, attempts))
        {
            history.bytes("historyhead", hash, 32);
            history.var("historycount", count);
            history.var("attempts", attempts);
        }
        addchunk(out, "hIST", history);

        sbr2::kvwriter prov;
        prov.boolean("live", true);
        prov.boolean("harness", run.harness);
        prov.boolean("dethash", run.dethash);
        prov.var("fixedframe", run.fixedframe);
        prov.boolean("edit", run.edit);
        addchunk(out, "pROV", prov);

        sbr2::kvwriter settings;
        loopi(sizeof(inputsettings)/sizeof(inputsettings[0])) addsetting(settings, inputsettings[i]);
        addchunk(out, "SETS", settings);

        sbr2::kvwriter window;
        window.var("focuslosses", run.focuslosses);
        window.var("focuslostms", run.focuslostus / 1000);
        window.var("minimizes", run.minimizes);
        defformatstring(resolution, "%dx%d", screenw, screenh);
        window.str("resolution", resolution);
        addsetting(window, "vsync");
        addsetting(window, "fullscreen");
        addchunk(out, "wIND", window);
        out.last()->late = true;

        sbr2::kvwriter rawstats;
        rawstatistics(r, rawstats);
        addchunk(out, "rSTA", rawstats);
        out.last()->late = true;
    }
}
