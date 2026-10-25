// Speedrun timer. Times are counted in simulation ticks, so a replay of an attempt shows
// exactly the same time as the run itself.
//
// Singleplayer: runs from the first tick of the map until endsp; a map change started by
// the map's own script carries the time over to the next map, load time excluded.
// Flagrun (CTF with flagrun rules): starts once the player holds a flag and has left its
// pickup sphere, ends when the player scores; losing the flag first re-arms it.
// Race (ffa or coop edit with race rules): starts at every spawn, ends when the player's feet
// touch any flag entity; entering edit mode voids the attempt.

#include "game.h"
#include "sbcompat.h"

namespace game
{
    enum { RUNTIMER_NONE = 0, RUNTIMER_SP, RUNTIMER_FLAG, RUNTIMER_RACE };
    // RUN_SPLIT: a campaign map's exit before the last map; the next map goes on from it
    enum { RUN_IDLE = 0, RUN_HOLDING, RUN_RUNNING, RUN_FINISHED, RUN_SPLIT };

    static int runkind = RUNTIMER_NONE, runstate = RUN_IDLE;
    static bool liferan = false;  // the run of player1's current life started (flagrun/race)
    enum { RACESTARTEDTICKS = 200 };
    static int runbase = 0, runstart = 0, runend = 0, runflag = -1;
    // a campaign run's deaths on the maps before this one; this map's are player1's own count
    static int rundeathsbefore = 0;
    static vec runflagloc(0, 0, 0);
    static float runflagradius = 0;

    void snapruntimer(snapio &s)
    {
        s.i(runkind); s.i(runstate); s.i(runbase); s.i(runstart); s.i(runend); s.i(runflag);
        s.v(runflagloc); s.f(runflagradius); s.n(liferan);
    }

    // flagruns and races are judged per life of player1
    bool runtimerperlife() { return runkind == RUNTIMER_FLAG || runkind == RUNTIMER_RACE; }
    bool runtimerliferan() { return liferan; }
    // tick the current run started at, -1 before it starts
    int runtimerstarttick() { return runstate >= RUN_RUNNING ? runstart : -1; }

    static int runticks()
    {
        if(runstate < RUN_RUNNING) return 0;
        return runbase + (runstate == RUN_FINISHED || runstate == RUN_SPLIT ? runend : inputtick()) - runstart;
    }

    // Campaigns: maps whose exits load the next map of the list. A run is finished at the last
    // map's exit; the exits before it are splits. The name is the single player menu's.
    static const struct { const char *name, *maps[5]; } campaigns[] =
    {
        { "An Army Of One", { "mpsp6a", "mpsp6b", "mpsp6c", NULL } },
        { "Private Stan Sauer", { "mpsp9a", "mpsp9b", "mpsp9c", "mpsp10", NULL } },
        { "Lost", { "lost", NULL } },
        { "Meltdown", { "skrsp1", NULL } },
        { "Missile Pass", { "crnsp1", NULL } },
        { "Level 9", { "level9", NULL } }
    };

    // the map's place in its campaign (-1 if it is in none) and the campaign's length
    int campaignmap(const char *map, int *length)
    {
        loopi(sizeof(campaigns)/sizeof(campaigns[0])) for(int j = 0; campaigns[i].maps[j]; j++) if(!strcmp(campaigns[i].maps[j], map))
        {
            if(length) { int n = 0; while(campaigns[i].maps[n]) n++; *length = n; }
            return j;
        }
        return -1;
    }

    // a run's name for people: the campaign's when the run starts on a campaign's first map
    const char *runlabel(const char *map)
    {
        loopi(sizeof(campaigns)/sizeof(campaigns[0])) if(!strcmp(campaigns[i].maps[0], map)) return campaigns[i].name;
        return map;
    }

    // the map start that follows a finished run starts a new run, even when a map script loads it
    static bool finishedatmapstart = false;
    bool runtimerfinishedbeforemap() { return finishedatmapstart; }

    const char *formatruntime(int ms)
    {
        static string buf;
        formatstring(buf, "%d:%02d.%03d", ms/60000, (ms/1000)%60, ms%1000);
        return buf;
    }

    void runtimermapstart(int mode, bool local)
    {
        bool sp = local && m_check(mode, M_DMSP | M_CLASSICSP);
        finishedatmapstart = runstate == RUN_FINISHED;
        bool chained = sp && runkind == RUNTIMER_SP && (runstate == RUN_RUNNING || runstate == RUN_SPLIT) && mapchained(mode);
        runbase = chained ? runbase + (runstate == RUN_SPLIT ? runend : inputtick()) - runstart : 0;
        rundeathsbefore = chained ? rundeathsbefore + player1->deaths : 0;
        runstart = runend = 0;
        runflag = -1;
        if(sp) { runkind = RUNTIMER_SP; runstate = RUN_RUNNING; }
        else if(local && m_checkonly(mode, M_CTF, M_PROTECT | M_HOLD) && server::flagrunactive()) { runkind = RUNTIMER_FLAG; runstate = RUN_IDLE; }
        else if(local && server::racerulesactive()) { runkind = RUNTIMER_RACE; runstate = RUN_IDLE; }
        else { runkind = RUNTIMER_NONE; runstate = RUN_IDLE; }
    }

    static void finishrun()
    {
        runend = inputtick();
        runstate = RUN_FINISHED;
        conoutf(CON_GAMEINFO, "\f2run time: %s", formatruntime(runticks()*SIMTICK));
        inputrunfinished(runticks()*SIMTICK);
        hubrunfinished(runticks()*SIMTICK);
    }

    void runtimerendsp()
    {
        if(runkind != RUNTIMER_SP || runstate != RUN_RUNNING) return;
        int length = 0, pos = campaignmap(getclientmap(), &length);
        if(pos >= 0 && pos < length-1)
        {
            runend = inputtick();
            runstate = RUN_SPLIT;
            conoutf(CON_GAMEINFO, "\f2%s done: %s (map %d of %d)", getclientmap(), formatruntime(runticks()*SIMTICK), pos+1, length);
            return;
        }
        finishrun();
    }

    void runtimerflagtaken(fpsent *d, int flag, const vec &loc, float radius)
    {
        if(d != player1 || runkind != RUNTIMER_FLAG || runstate == RUN_RUNNING) return;
        runstate = RUN_HOLDING;
        liferan = true;
        runflag = flag;
        runflagloc = loc;
        runflagradius = radius;
        runbase = runstart = runend = 0;
    }

    void runtimerflagscored(fpsent *d)
    {
        if(d == player1 && runkind == RUNTIMER_FLAG && runstate == RUN_RUNNING) finishrun();
    }

    // Race goals as racing servers detect them: a flag or player start with a negative team
    // (attr2) is the centre of a cube of half-size attr3 (24 if not positive), and the player's eye
    // has to be strictly inside it at least 3 s after spawning.
    enum { RACEGOALSIZE = 24, RACEGOALMINTICKS = 3000/SIMTICK };

    static bool inracegoal(const vec &eye)
    {
        const vector<extentity *> &ents = entities::getents();
        loopv(ents)
        {
            const extentity &e = *ents[i];
            if((e.type != FLAG && e.type != PLAYERSTART) || e.attr2 >= 0) continue;
            float r = e.attr3 > 0 ? e.attr3 : RACEGOALSIZE;
            if(fabs(eye.x - e.o.x) < r && fabs(eye.y - e.o.y) < r && fabs(eye.z - e.o.z) < r) return true;
        }
        return false;
    }

    void runtimerspawned(fpsent *d)
    {
        if(d != player1 || !runtimerperlife()) return;
        inputlifespawned();
        liferan = false;
        if(runkind != RUNTIMER_RACE) return;
        runstate = RUN_RUNNING;
        runbase = runend = 0;
        runstart = inputtick();
    }

    void runtimeredittoggled(bool on)
    {
        if(!on || runkind != RUNTIMER_RACE || runstate != RUN_RUNNING) return;
        runstate = RUN_IDLE;
        conoutf(CON_GAMEINFO, "\f3race attempt voided: edit mode used");
    }

    static void racetick()
    {
        if(runstate != RUN_RUNNING || player1->state != CS_ALIVE) return;
        if(runticks() >= RACESTARTEDTICKS) liferan = true;
        if(runticks() > RACEGOALMINTICKS && inracegoal(player1->o)) finishrun();
    }

    int runtimerticks(bool &running) { running = runstate == RUN_RUNNING; return runticks(); }
    bool runtimerfinished() { return runstate == RUN_FINISHED; }

    // the run kind as the statistics name it, NULL when nothing is timed
    const char *runtimerkindname()
    {
        switch(runkind)
        {
            case RUNTIMER_SP: return "sp";
            case RUNTIMER_FLAG: return "flagrun";
            case RUNTIMER_RACE: return "race";
            default: return NULL;
        }
    }

    // flagruns: walking back into the flag's pickup sphere puts the run back to holding, so the
    // timer starts again when the player leaves it (a replay-forced sim setting)
    MOD(VARP, flagrunrestart, 0, 0, 1);

    static void updaterun()
    {
        if(runkind == RUNTIMER_RACE) { racetick(); return; }
        if(runkind != RUNTIMER_FLAG || (runstate != RUN_HOLDING && runstate != RUN_RUNNING)) return;
        if(!flagheldby(runflag, player1)) { runstate = RUN_IDLE; return; }
        float dist = player1->feetpos().dist(runflagloc);
        if(runstate == RUN_HOLDING && dist >= runflagradius)
        {
            runstate = RUN_RUNNING;
            runstart = inputtick();
        }
        else if(runstate == RUN_RUNNING && flagrunrestart && dist < runflagradius)
        {
            runstate = RUN_HOLDING;
            runbase = runstart = runend = 0;
        }
    }

    // called at the end of every simulated tick
    void runtimertick()
    {
        updaterun();
        ghosttick();
        splittick();
        spawntick();
    }

    int runtimerresult(bool &finished)
    {
        finished = runstate == RUN_FINISHED;
        return runticks()*SIMTICK;
    }

    int runtimerdeaths() { return runkind == RUNTIMER_SP ? rundeathsbefore + player1->deaths : 0; }

    ICOMMAND(runtime, "", (), intret(runticks()*SIMTICK));
    ICOMMAND(rundeaths, "", (), intret(runtimerdeaths()));
    ICOMMAND(runstate, "", (), intret(runkind == RUNTIMER_NONE ? -1 : runstate));

    MOD(VARP, runtimer, 0, 1, 1);
    MOD(FVARP, runtimerscale, 0.5, 1.0, 2.0);
    MOD(FVARP, runtimerx, 0.0, 0.50, 1.0);
    MOD(FVARP, runtimery, 0.0, 0.08, 1.0);
    MOD(HVARP, runtimercolour, 0, 0xFFFFFF, 0xFFFFFF);
    MOD(HVARP, runtimeridlecolour, 0, 0x808080, 0xFFFFFF);
    MOD(HVARP, runtimerpbcolour, 0, 0x40FF80, 0xFFFFFF);
    MOD(HVARP, runtimerfastercolour, 0, 0xFFFF40, 0xFFFFFF);
    MOD(HVARP, runtimerslowercolour, 0, 0xFF8040, 0xFFFFFF);
    MOD(VARP, runtimerrecord, 0, 1, 1);
    MOD(HVARP, runtimerrecordcolour, 0, 0xB0B0B0, 0xFFFFFF);
    MOD(VARP, runtimerdeathcount, 0, 1, 1);
    ICOMMAND(hexcolour, "i", (int *colour), { defformatstring(hex, "%06X", *colour & 0xFFFFFF); result(hex); });

    void drawruntimer(int w, int h)
    {
        if(!runtimer || runkind == RUNTIMER_NONE) return;
        const char *buf = formatruntime(runticks()*SIMTICK);
        int tw = 0, th = 0;
        text_bounds(buf, tw, th);
        vec2 offset = vec2(runtimerx, runtimery).mul(vec2(w, h).div(runtimerscale));
        offset.x -= tw/2.0f;
        offset.y -= th/2.0f;

        pushhudmatrix();
        hudmatrix.scale(runtimerscale, runtimerscale, 1);
        flushhudmatrix();
        int color = runtimeridlecolour;
        if(runstate == RUN_RUNNING) color = runtimercolour;
        else if(runstate == RUN_FINISHED) switch(splitsfinishresult())
        {
            case FINISH_FASTER: color = runtimerfastercolour; break;
            case FINISH_SLOWER: color = runtimerslowercolour; break;
            default: color = runtimerpbcolour; break;
        }
        draw_text(buf, int(offset.x), int(offset.y), (color>>16)&0xFF, (color>>8)&0xFF, color&0xFF, 255);
        pophudmatrix();

        // the board's best under the timer, at half its size
        const char *rec = runtimerrecord ? hubboardhud() : NULL;
        if(rec)
        {
            float scale = runtimerscale*0.5f;
            int rw = 0, rh = 0;
            text_bounds(rec, rw, rh);
            vec2 at = vec2(runtimerx*w, runtimery*h + th*runtimerscale*0.55f).div(scale);
            pushhudmatrix();
            hudmatrix.scale(scale, scale, 1);
            flushhudmatrix();
            draw_text(rec, int(at.x - rw/2.0f), int(at.y), (runtimerrecordcolour>>16)&0xFF, (runtimerrecordcolour>>8)&0xFF, runtimerrecordcolour&0xFF, 255);
            pophudmatrix();
        }

    }

    // a campaign run's deaths, on top of the bottom right stack
    void hudstack(int right, int top)
    {
        int ndeaths = runtimerdeaths();
        if(!runtimerdeathcount || ndeaths <= 0) return;
        defformatstring(deaths, "%d death%s", ndeaths, ndeaths == 1 ? "" : "s");
        int tw = 0, th = 0;
        text_bounds(deaths, tw, th);
        draw_text(deaths, right - tw, top, (runtimerrecordcolour>>16)&0xFF, (runtimerrecordcolour>>8)&0xFF, runtimerrecordcolour&0xFF, 255);
    }
}
