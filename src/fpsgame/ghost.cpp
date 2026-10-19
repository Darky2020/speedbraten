// PB ghost: while a run is timed, player1's path is sampled on the run timer's clock and saved
// with the run's replay when it finishes. The ghost of the PB replay (or one picked by hand) is
// drawn at the live run time. The ghost is not a game entity; it never touches the simulation.

#include "game.h"
#include "inputs.h"

namespace game
{
    VARP(ghost, 0, 1, 1);     // draw the ghost
    VARP(ghostdiff, 0, 1, 1); // show the live time difference to it
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
    static string ghostfile = "", ghostkey = "", ghostlabel = "";
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

    // Picks the ghost again whenever the run, its category or the statistics change: the replay
    // chosen for the map, kind and mode, else the PB (the segmented PB in a segmented run, with
    // ghostsegpb, or when chosen).
    static void chooseghost()
    {
        const inputattempt *first = inputrunfirst();
        const char *kind = runtimerkindname();
        if(!first || !kind) { ghostkey[0] = '\0'; clearghost(); return; }
        bool segmented = inputsplicecount() > 0;
        defformatstring(key, "%s %s %d %d %d %d", first->map, kind, first->mode, segmented ? 1 : 0, ghostsegpb, statsversion());
        if(!strcmp(key, ghostkey)) return;
        copystring(ghostkey, key);
        string source = "pb", file = "", label = "";
        statsghostchoice(first->map, kind, first->mode, source, file, label);
        if(!strcmp(source, "file") && loadghost(file))
        {
            copystring(ghostlabel, label[0] ? label : "ghost");
            return;
        }
        bool seg = segmented || ghostsegpb || !strcmp(source, "segpb");
        if(statspbreplay(first->map, kind, first->mode, seg, file)) {}
        else if(statspbreplay(first->map, kind, first->mode, !seg, file)) seg = !seg;
        else { clearghost(); return; }
        loadghost(file);
        copystring(ghostlabel, label[0] ? label : seg ? "seg PB" : "PB");
    }

    const char *ghostname() { return ghostlabel; }

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
        if(!ghostdiff || samples.empty() || !runtimerkindname()) return false;
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
        if(!havedelta || !ghostdiff) return false;
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
        if(mainpass) particle_textcopy(d.abovehead(), ghostlabel, PART_TEXT, 1, 0x80FFFF, 2.0f);
    }

    static const char *replaykindof(const replayinfo &info) { return info.race ? "race" : info.flagrun ? "flagrun" : "sp"; }

    // ghostrace file [label]: races that replay on its map, kind and mode from now on
    static void ghostrace(const char *file, const char *label)
    {
        inputrun run;
        replayinfo info;
        if(!readreplay(file, &run, info)) { conoutf(CON_ERROR, "could not read %s", file); return; }
        bool path = false;
        loopv(run.attempts) if(run.attempts[i]->trace.length()) path = true;
        if(!path) { conoutf(CON_ERROR, "%s has no ghost path: only replays made with speedbraten v0.1.0 or later can be ghosts", file); return; }
        string name;
        if(label[0]) copystring(name, label);
        else if(info.finished) formatstring(name, "%s %d:%02d.%03d", info.player, info.runtime/60000, (info.runtime/1000)%60, info.runtime%1000);
        else copystring(name, info.player);
        statssetghost(info.map, replaykindof(info), info.mode, "file", file, name);
        conoutf("ghost on %s (%s): %s", info.map, server::modename(info.mode, "?"), name);
    }
    ICOMMAND(ghostrace, "ss", (char *file, char *label), ghostrace(file, label));
    ICOMMAND(ghostuse, "s", (char *file), ghostrace(file, ""));

    // the current run's category, for the commands below
    static bool currentcategory(const char *&map, const char *&kind, int &mode)
    {
        const inputattempt *first = inputrunfirst();
        kind = runtimerkindname();
        if(!first || !kind) return false;
        map = first->map;
        mode = first->mode;
        return true;
    }

    // ghostsource pb|segpb: races the PB or the segmented PB on the current map and mode
    static void ghostsource(const char *source)
    {
        const char *map, *kind;
        int mode;
        if(!currentcategory(map, kind, mode)) { conoutf(CON_ERROR, "ghostsource: start a run on the map first"); return; }
        statssetghost(map, kind, mode, !strcmp(source, "segpb") ? "segpb" : "pb", NULL, NULL);
    }
    ICOMMAND(ghostsource, "s", (char *source), ghostsource(source));

    // ghostlabel text: renames the ghost of the current map and mode ("" for the default)
    static void setghostlabel(const char *label)
    {
        const char *map, *kind;
        int mode;
        if(!currentcategory(map, kind, mode)) { conoutf(CON_ERROR, "ghostlabel: start a run on the map first"); return; }
        string source = "pb", file = "", old = "";
        statsghostchoice(map, kind, mode, source, file, old);
        statssetghost(map, kind, mode, source, file, label);
    }
    ICOMMAND(ghostlabel, "s", (char *label), setghostlabel(label));

    // ghostchoice field: source, file or label of the current map and mode's ghost, and name
    // (the label shown)
    static void ghostchoicefield(const char *field)
    {
        const char *map, *kind;
        int mode;
        string source = "pb", file = "", label = "";
        if(!currentcategory(map, kind, mode)) { result(""); return; }
        statsghostchoice(map, kind, mode, source, file, label);
        if(!strcmp(field, "source")) result(source);
        else if(!strcmp(field, "file")) result(file);
        else if(!strcmp(field, "label")) result(label);
        else if(!strcmp(field, "name")) result(ghostlabel);
        else result("");
    }
    ICOMMAND(ghostchoice, "s", (char *field), ghostchoicefield(field));

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
