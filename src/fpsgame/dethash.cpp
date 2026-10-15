// Determinism harness: writes one line of grouped game-state hashes per simulated frame,
// and provides input commands that scripts can drive without real key presses.

#include "game.h"

namespace game
{
    static stream *dethashfile = NULL;
    static int dethashframes = 0;
    static string dethashonstart = "";

    static void dethashstop()
    {
        if(!dethashfile) return;
        DELETEP(dethashfile);
        conoutf("dethash: wrote %d frames", dethashframes);
    }
    COMMAND(dethashstop, "");

    // onstart names an alias executed after the first simulated frame, so scripted input
    // is timed from game start no matter how many frames map loading took.
    ICOMMAND(dethashstart, "ss", (char *file, char *onstart),
    {
        dethashstop();
        dethashfile = openutf8file(path(file, true), "w");
        if(!dethashfile) { conoutf(CON_ERROR, "dethash: could not open %s", file); return; }
        dethashframes = 0;
        copystring(dethashonstart, onstart);
        dethashfile->printf("# frame lastmillis player monsters projectiles movables entities rng\n");
    });

    ICOMMAND(harnessaim, "ff", (float *yaw, float *pitch), { player1->yaw = *yaw; player1->pitch = *pitch; });
    ICOMMAND(harnessmove, "ii", (int *move, int *strafe), { player1->move = clamp(*move, -1, 1); player1->strafe = clamp(*strafe, -1, 1); });
    ICOMMAND(harnessjump, "", (), { if(canjump()) player1->jumping = true; });
    ICOMMAND(harnessattack, "i", (int *down), { doattack(*down != 0); });

    void dethashframe()
    {
        if(!dethashfile) return;

        dethasher pl;
        pl.phys(player1);
        pl.i(player1->health); pl.i(player1->armour); pl.i(player1->armourtype);
        pl.i(player1->gunselect); pl.i(player1->gunwait); pl.i(player1->quadmillis);
        loopi(NUMGUNS) pl.i(player1->ammo[i]);
        pl.i(player1->lastaction); pl.i(player1->lastpain); pl.i(player1->lifesequence);
        pl.i(player1->lasttaunt); pl.i(player1->attacking);

        dethasher mo, pr, mv, en;
        hashmonsters(mo);
        hashprojectiles(pr);
        hashmovables(mv);

        const vector<extentity *> &ents = entities::getents();
        loopv(ents)
        {
            fpsentity &e = *(fpsentity *)ents[i];
            en.i(e.type); en.i(e.spawned()); en.i(e.triggerstate); en.i(e.lasttrigger);
        }

        dethashfile->printf("%d %d %08x %08x %08x %08x %08x %08x\n",
            dethashframes++, lastmillis, pl.h, mo.h, pr.h, mv.h, en.h, hashMT(2166136261U));

        if(dethashframes == 1 && dethashonstart[0]) execident(dethashonstart);
    }
}
