// PB ghost: while a run is timed, player1's path is sampled on the run timer's clock and saved
// with the run's replay when it finishes. The ghost of the PB replay (or one picked by hand) is
// drawn at the live run time. The ghost is not a game entity; it never touches the simulation.

#include "game.h"
#include "inputs.h"

namespace game
{
    VARP(ghost, 0, 1, 1);
    VARP(ghostsegpb, 0, 0, 1);
    FVARP(ghostalpha, 0.1f, 0.4f, 1);

    enum { SAMPLETICKS = 2 };
    static const float GHOSTHIDEDIST = 8, GHOSTFADEDIST = 16;

    // the current run's samples, with the index of the chained map each was taken on
    static vector<ghostsample> live;
    static vector<int> livemaps;

    void ghostattach(inputrun &run)
    {
        loopv(run.attempts) run.attempts[i]->trace.setsize(0);
        loopv(live) if(run.attempts.inrange(livemaps[i])) run.attempts[livemaps[i]]->trace.add(live[i]);
    }

    static void recordsample()
    {
        bool running;
        int t = runtimerticks(running);
        if(!running || player1->state != CS_ALIVE || t % SAMPLETICKS) return;
        // a state load or a new run moves the clock back
        while(live.length() && live.last().tick >= t) { live.pop(); livemaps.pop(); }
        ghostsample &g = live.add();
        g.tick = t;
        g.o = player1->o;
        g.yaw = player1->yaw;
        g.pitch = player1->pitch;
        g.flags = ((player1->move+1) << ghostsample::MOVESHIFT) | ((player1->strafe+1) << ghostsample::STRAFESHIFT) |
                  (player1->inwater ? ghostsample::INWATER : 0) | (player1->timeinair > 100 ? ghostsample::INAIR : 0) |
                  (player1->lastaction > lastmillis - SAMPLETICKS*SIMTICK ? ghostsample::ATTACK : 0) |
                  (player1->gunselect << ghostsample::GUNSHIFT);
        livemaps.add(inputattemptindex());
    }

    // the ghost being shown: every attempt's samples in run order, and the map of each
    static inputrun ghostrun;
    static string ghostfile = "", ghostkey = "", pickedmap = "";
    static bool ghostpicked = false;
    static vector<ghostsample> samples;
    static vector<int> samplemaps;

    static bool loadghost(const char *file)
    {
        if(!strcmp(file, ghostfile)) return samples.length() > 0;
        copystring(ghostfile, file);
        samples.setsize(0);
        samplemaps.setsize(0);
        ghostrun.clear();
        replayinfo info;
        if(!file[0] || !readreplay(file, &ghostrun, info)) return false;
        loopvk(ghostrun.attempts) loopv(ghostrun.attempts[k]->trace)
        {
            samples.add(ghostrun.attempts[k]->trace[i]);
            samplemaps.add(k);
        }
        return samples.length() > 0;
    }

    static void clearghost()
    {
        ghostfile[0] = '\0';
        samples.setsize(0);
        samplemaps.setsize(0);
        ghostrun.clear();
    }

    // picks the ghost again whenever the run, its category or the statistics change
    static void chooseghost()
    {
        const inputattempt *first = inputrunfirst();
        const char *kind = runtimerkindname();
        if(!first || !kind) { ghostkey[0] = '\0'; ghostpicked = false; clearghost(); return; }
        if(ghostpicked)
        {
            if(!strcmp(pickedmap, first->map)) return;
            ghostpicked = false;
            ghostkey[0] = '\0';
        }
        bool segmented = inputsplicecount() > 0;
        defformatstring(key, "%s %s %d %d %d %d", first->map, kind, first->mode, segmented ? 1 : 0, ghostsegpb, statsversion());
        if(!strcmp(key, ghostkey)) return;
        copystring(ghostkey, key);
        string file;
        bool seg = segmented || ghostsegpb;
        if(statspbreplay(first->map, kind, first->mode, seg, file) || statspbreplay(first->map, kind, first->mode, !seg, file)) loadghost(file);
        else clearghost();
    }

    // the ghost's sample at run tick t on the current map, interpolated; false if none
    static bool ghostat(int t, ghostsample &out)
    {
        if(samples.empty()) return false;
        int map = inputattemptindex();
        int lo = 0, hi = samples.length();
        while(hi - lo > 1)
        {
            int mid = (lo + hi) / 2;
            if(samples[mid].tick <= t) lo = mid; else hi = mid;
        }
        if(samplemaps[lo] != map)
        {
            // before this map's first sample: wait there
            int first = samplemaps.find(map);
            if(first < 0) return false;
            lo = first;
        }
        if(!ghostrun.attempts.inrange(map) || strcmp(ghostrun.attempts[map]->map, getclientmap())) return false;
        out = samples[lo];
        if(lo + 1 < samples.length() && samplemaps[lo+1] == map && samples[lo].tick < t && samples[lo+1].tick > samples[lo].tick)
        {
            const ghostsample &next = samples[lo+1];
            float f = clamp(float(t - samples[lo].tick) / (next.tick - samples[lo].tick), 0.0f, 1.0f);
            out.o.lerp(samples[lo].o, next.o, f);
            float dyaw = next.yaw - samples[lo].yaw;
            if(dyaw > 180) dyaw -= 360; else if(dyaw < -180) dyaw += 360;
            out.yaw = samples[lo].yaw + dyaw*f;
            out.pitch = samples[lo].pitch + (next.pitch - samples[lo].pitch)*f;
        }
        return true;
    }

    static bool currentghost(ghostsample &g)
    {
        if(!ghost || !connected || !runtimerkindname()) return false;
        bool running;
        int t = runtimerticks(running);
        return ghostat(running || t > 0 ? t : 0, g);
    }

    enum { DELTAWINDOW = 600 };      // ticks searched either side of the run time
    static const float DELTARANGE = 64; // farther from the ghost's path than this: no delta

    // How far behind the ghost the player is, in ticks (negative: ahead): the run time minus
    // the time at which the ghost passed the point of its path closest to the player. Taken at
    // the end of a tick, where player1 is at its simulated position.
    static bool computedelta(int &delta)
    {
        if(!ghost || samples.empty() || !runtimerkindname()) return false;
        bool running;
        int t = runtimerticks(running);
        if(!running) return false;
        int map = inputattemptindex();
        vec p = player1->o;
        float bestdist = DELTARANGE;
        int bestt = -1;
        loopv(samples)
        {
            if(samplemaps[i] != map || samples[i].tick < t - DELTAWINDOW) continue;
            if(samples[i].tick > t + DELTAWINDOW) break;
            const ghostsample &a = samples[i];
            if(i + 1 < samples.length() && samplemaps[i+1] == map)
            {
                const ghostsample &b = samples[i+1];
                vec ab = vec(b.o).sub(a.o);
                float len2 = ab.squaredlen(), f = len2 > 0 ? clamp(vec(p).sub(a.o).dot(ab) / len2, 0.0f, 1.0f) : 0;
                float dist = vec(ab).mul(f).add(a.o).dist(p);
                if(dist < bestdist) { bestdist = dist; bestt = a.tick + int(f*(b.tick - a.tick) + 0.5f); }
            }
            else if(a.o.dist(p) < bestdist) { bestdist = a.o.dist(p); bestt = a.tick; }
        }
        if(bestt < 0) return false;
        delta = t - bestt;
        return true;
    }

    static int lastdelta = 0;
    static bool havedelta = false;

    bool ghostdelta(int &delta)
    {
        if(!havedelta || !ghost) return false;
        delta = lastdelta;
        return true;
    }

    void ghosttick()
    {
        if(inputrecordinglive()) recordsample();
        chooseghost();
        havedelta = computedelta(lastdelta);
    }

    static fpsent *ghostent = NULL;

    void renderghost(bool mainpass)
    {
        ghostsample g;
        if(!currentghost(g)) return;
        // fades out as the camera gets inside it
        float alpha = ghostalpha * clamp((camera1->o.dist(g.o) - GHOSTHIDEDIST) / GHOSTFADEDIST, 0.0f, 1.0f);
        if(alpha <= 0) return;
        if(!ghostent) ghostent = new fpsent;
        fpsent &d = *ghostent;
        // a map start or a state load moves the clock back; animation blending from timestamps
        // in the future would extrapolate the mesh
        if(lastmillis < d.lastrendered || lastmillis < d.lastaction)
        {
            loopi(MAXANIMPARTS) d.animinterp[i].reset();
            d.lastrendered = 0;
            d.lastaction = 0;
        }
        d.o = g.o;
        d.yaw = g.yaw;
        d.pitch = g.pitch;
        d.move = ((g.flags >> ghostsample::MOVESHIFT) & 3) - 1;
        d.strafe = ((g.flags >> ghostsample::STRAFESHIFT) & 3) - 1;
        d.inwater = g.flags & ghostsample::INWATER ? 1 : 0;
        d.timeinair = g.flags & ghostsample::INAIR ? 200 : 0;
        d.physstate = g.flags & ghostsample::INAIR ? PHYS_FALL : PHYS_FLOOR;
        d.gunselect = clamp(g.flags >> ghostsample::GUNSHIFT, 0, int(GUN_PISTOL));
        if(g.flags & ghostsample::ATTACK) { d.lastaction = lastmillis; d.lastattackgun = d.gunselect; }
        d.state = CS_ALIVE;
        d.lastpain = 0;
        d.armour = 0;
        renderplayer(&d, getplayermodelinfo(player1), 0, alpha, mainpass);
        if(mainpass) particle_text(d.abovehead(), ghostpicked ? "ghost" : "PB", PART_TEXT, 1, 0x80FFFF, 2.0f);
    }

    // ghostuse file: shows that replay's ghost until a run on another map starts
    ICOMMAND(ghostuse, "s", (char *file),
    {
        const inputattempt *first = inputrunfirst();
        ghostfile[0] = '\0';
        if(!loadghost(file)) { conoutf(CON_ERROR, "%s has no ghost (replays made before ghosts were added have none)", file); return; }
        ghostpicked = true;
        copystring(pickedmap, first ? first->map : "");
        conoutf("ghost: %s", file);
    });

    // harness: the ghost's sample count and its position now
    ICOMMAND(ghostinfo, "", (),
    {
        ghostsample g;
        if(!currentghost(g)) { defformatstring(res, "samples=%d none", samples.length()); result(res); return; }
        defformatstring(res, "samples=%d pos=%.3f %.3f %.3f animswitch=%d rendered=%d now=%d", samples.length(), g.o.x, g.o.y, g.o.z,
                        ghostent ? ghostent->animinterp[0].lastswitch : -1, ghostent ? ghostent->lastrendered : -1, lastmillis);
        result(res);
    });
}
