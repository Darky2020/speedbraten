// Offline, the game advances in fixed SIMTICK steps of game time, decoupled from frame
// rate, so identical inputs give identical results on any machine and frame pacing.
// The main loop calls simframestart, simadvanceclock, simupdate and simdisplaystart once per frame.

#include "engine.h"

bool simticking = false, insimtick = false;
static bool simrestarted = false;
static int simaccum = 0;
static bool simclockskip = false;

// Puts the game clock at a fixed value and drops the time spent until the next frame
// (e.g. map loading), so a new local game always starts from the same clock state.
void resetsimclock(int base)
{
    extern int lastphysframe;
    int delta = base - lastmillis;
    lastmillis = base;
    lastphysframe = base;
    rebasesleeps(delta);
    simaccum = 0;
    simclockskip = true;
    simrestarted = true;
}

// before the frame's game time is scaled from elapsedtime
void simframestart(int &timeerr)
{
    simticking = !multiplayer(false);
    if(simticking && simclockskip) { elapsedtime = 0; timeerr = 0; simclockskip = false; }
}

void simadvanceclock()
{
    if(simticking)
    {
        if(curtime>200) curtime = 200;
        if(!game::ispaused() && !game::holdsim()) simaccum += curtime;
    }
    else if(!game::ispaused()) lastmillis += curtime;
}

void simupdate(bool draw)
{
    if(simticking)
    {
        int simulated = 0;
        while(simaccum >= SIMTICK && !game::ispaused() && !game::holdsim())
        {
            simaccum -= SIMTICK;
            curtime = SIMTICK;
            lastmillis += SIMTICK;
            simulated += SIMTICK;
            // a map start inside a tick abandons the rest of it, so every way of
            // starting a game leads to the same first tick
            simrestarted = false;
            insimtick = true;
            detfpmode();
            game::begintick();
            if(!simrestarted) game::updateworld();
            if(!simrestarted) checksleep(lastmillis);
            if(!simrestarted) serverslice(false, 0);
            insimtick = false;
            if(simrestarted || !simticking) break;
        }
        curtime = simulated;
#ifndef HEADLESS
        static Uint64 lastframe = 0;
        Uint64 now = SDL_GetPerformanceCounter();
        if(lastframe) game::frametimed(int(min((now - lastframe)*1000000/SDL_GetPerformanceFrequency(), Uint64(1)<<30)), simulated/SIMTICK, draw);
        lastframe = now;
#endif
    }
    else
    {
        if(lastmillis) game::updateworld();

        checksleep(lastmillis);

        serverslice(false, 0);
    }
}

// lets the next frames simulate ms of game time on top of the time that passed, e.g. to move a replay ahead
void simfastforward(int ms)
{
    if(simticking && ms > 0) simaccum += ms - ms%SIMTICK;
}

// true when the frame shows the simulation between ticks; then game::enddisplay must follow
bool simdisplaystart()
{
    bool display = simticking && !mainmenu;
    if(display) game::begindisplay(simaccum/float(SIMTICK));
    return display;
}
