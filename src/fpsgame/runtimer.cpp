// Speedrun timer. Times are counted in simulation ticks, so a replay of an attempt shows
// exactly the same time as the run itself.
//
// Singleplayer: runs from the first tick of the map until endsp; a map change started by
// the map's own script carries the time over to the next map, load time excluded.
// Flagrun (CTF with flagrun rules): starts once the player holds a flag and has left its
// pickup sphere, ends when the player scores; losing the flag first re-arms it.

#include "game.h"

namespace game
{
    enum { RUNTIMER_NONE = 0, RUNTIMER_SP, RUNTIMER_FLAG };
    enum { RUN_IDLE = 0, RUN_HOLDING, RUN_RUNNING, RUN_FINISHED };

    static int runkind = RUNTIMER_NONE, runstate = RUN_IDLE;
    static int runbase = 0, runstart = 0, runend = 0, runflag = -1;
    static vec runflagloc(0, 0, 0);
    static float runflagradius = 0;

    static int runticks()
    {
        if(runstate < RUN_RUNNING) return 0;
        return runbase + (runstate == RUN_FINISHED ? runend : inputtick()) - runstart;
    }

    static const char *formatruntime(int ms)
    {
        static string buf;
        formatstring(buf, "%d:%02d.%03d", ms/60000, (ms/1000)%60, ms%1000);
        return buf;
    }

    void runtimermapstart(int mode, bool local)
    {
        bool sp = local && m_check(mode, M_DMSP | M_CLASSICSP);
        bool chained = sp && runkind == RUNTIMER_SP && runstate == RUN_RUNNING && mapchained(mode);
        runbase = chained ? runbase + inputtick() - runstart : 0;
        runstart = runend = 0;
        runflag = -1;
        if(sp) { runkind = RUNTIMER_SP; runstate = RUN_RUNNING; }
        else if(local && m_checkonly(mode, M_CTF, M_PROTECT | M_HOLD) && server::flagrunactive()) { runkind = RUNTIMER_FLAG; runstate = RUN_IDLE; }
        else { runkind = RUNTIMER_NONE; runstate = RUN_IDLE; }
    }

    static void finishrun()
    {
        runend = inputtick();
        runstate = RUN_FINISHED;
        conoutf(CON_GAMEINFO, "\f2run time: %s", formatruntime(runticks()*SIMTICK));
        inputrunfinished(runticks()*SIMTICK);
    }

    void runtimerendsp()
    {
        if(runkind == RUNTIMER_SP && runstate == RUN_RUNNING) finishrun();
    }

    void runtimerflagtaken(fpsent *d, int flag, const vec &loc, float radius)
    {
        if(d != player1 || runkind != RUNTIMER_FLAG || runstate == RUN_RUNNING) return;
        runstate = RUN_HOLDING;
        runflag = flag;
        runflagloc = loc;
        runflagradius = radius;
        runbase = runstart = runend = 0;
    }

    void runtimerflagscored(fpsent *d)
    {
        if(d == player1 && runkind == RUNTIMER_FLAG && runstate == RUN_RUNNING) finishrun();
    }

    // called at the end of every simulated tick
    void runtimertick()
    {
        if(runkind != RUNTIMER_FLAG || (runstate != RUN_HOLDING && runstate != RUN_RUNNING)) return;
        if(!flagheldby(runflag, player1)) { runstate = RUN_IDLE; return; }
        if(runstate == RUN_HOLDING && player1->feetpos().dist(runflagloc) >= runflagradius)
        {
            runstate = RUN_RUNNING;
            runstart = inputtick();
        }
    }

    int runtimerresult(bool &finished)
    {
        finished = runstate == RUN_FINISHED;
        return runticks()*SIMTICK;
    }

    ICOMMAND(runtime, "", (), intret(runticks()*SIMTICK));
    ICOMMAND(runstate, "", (), intret(runkind == RUNTIMER_NONE ? -1 : runstate));

    VARP(runtimer, 0, 1, 1);
    FVARP(runtimerscale, 0.5, 1.0, 2.0);
    FVARP(runtimerx, 0.0, 0.50, 1.0);
    FVARP(runtimery, 0.0, 0.08, 1.0);

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
        int color = runstate == RUN_FINISHED ? 0x40FF80 : (runstate == RUN_RUNNING ? 0xFFFFFF : 0x808080);
        draw_text(buf, int(offset.x), int(offset.y), (color>>16)&0xFF, (color>>8)&0xFF, color&0xFF, 255);
        pophudmatrix();
    }
}
