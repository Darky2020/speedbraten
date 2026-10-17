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
        snapcyclereport();
    }
    COMMAND(dethashstop, "");

    bool dethashactive() { return dethashfile != NULL; }

    // onstart names an alias executed after the first simulated frame, so scripted input
    // is timed from game start no matter how many frames map loading took.
    ICOMMAND(dethashstart, "ss", (char *file, char *onstart),
    {
        dethashstop();
        dethashfile = openutf8file(path(file, true), "w");
        if(!dethashfile) { conoutf(CON_ERROR, "dethash: could not open %s", file); return; }
        dethashframes = 0;
        copystring(dethashonstart, onstart);
        dethashfile->printf("# frame lastmillis player monsters projectiles movables entities rng server actors world\n");
    });

    ICOMMAND(harnessaim, "ff", (float *yaw, float *pitch), { player1->yaw = *yaw; player1->pitch = *pitch; });
    ICOMMAND(harnessmove, "ii", (int *move, int *strafe), { player1->move = clamp(*move, -1, 1); player1->strafe = clamp(*strafe, -1, 1); });
    ICOMMAND(harnessjump, "", (), queueinput(INPUT_JUMP, 1));
    ICOMMAND(harnessattack, "i", (int *down), queueinput(INPUT_ATTACK, *down != 0 ? 1 : 0));
    ICOMMAND(harnessflag, "if", (int *own, float *dx),
    {
        vec loc;
        if(!flagspawnloc(*own != 0, loc)) { conoutf(CON_ERROR, "harnessflag: no flag"); return; }
        player1->o = loc.add(vec(*dx, 0, player1->eyeheight));
        player1->vel = vec(0, 0, 0);
        player1->resetinterp();
    });
    ICOMMAND(harnessstate, "", (), intret(player1->state));
    ICOMMAND(harnessinput, "", (),
    {
        defformatstring(res, "move=%d strafe=%d attacking=%d", player1->move, player1->strafe, player1->attacking ? 1 : 0);
        result(res);
    });
    // prints every player1 field that goes into the state hash
    ICOMMAND(harnessplayer, "", (),
    {
        fpsent *d = player1;
        conoutf("player o %.3f %.3f %.3f vel %.3f %.3f %.3f falling %.3f %.3f %.3f floor %.3f %.3f %.3f yaw %.3f pitch %.3f",
            d->o.x, d->o.y, d->o.z, d->vel.x, d->vel.y, d->vel.z, d->falling.x, d->falling.y, d->falling.z, d->floor.x, d->floor.y, d->floor.z, d->yaw, d->pitch);
        conoutf("player newpos %.3f %.3f %.3f timeinair %d inwater %d jumping %d move %d strafe %d physstate %d state %d blocked %d",
            d->newpos.x, d->newpos.y, d->newpos.z, d->timeinair, d->inwater, d->jumping, d->move, d->strafe, d->physstate, d->state, d->blocked);
        conoutf("player health %d armour %d armourtype %d gun %d gunwait %d quad %d lastaction %d lastpain %d lifesequence %d lasttaunt %d attacking %d",
            d->health, d->armour, d->armourtype, d->gunselect, d->gunwait, d->quadmillis, d->lastaction, d->lastpain, d->lifesequence, d->lasttaunt, d->attacking);
        string ammo = "";
        loopi(NUMGUNS) concformatstring(ammo, " %d", d->ammo[i]);
        conoutf("player ammo%s", ammo);
    });
    // spawned pickup items on the map
    ICOMMAND(harnessitems, "", (),
    {
        const vector<extentity *> &ents = entities::getents();
        int n = 0;
        loopv(ents) if(ents[i]->type >= I_SHELLS && ents[i]->type <= I_QUAD && ents[i]->spawned()) n++;
        intret(n);
    });
    ICOMMAND(harnesscommand, "s", (char *cmd), queueinput(INPUT_COMMAND, 0, cmd));
    // moves player1 onto the n-th entity of the given type; not recorded, so a replay of the
    // attempt does not reproduce it
    ICOMMAND(harnessgotoent, "ii", (int *type, int *n),
    {
        const vector<extentity *> &ents = entities::getents();
        int k = 0;
        loopv(ents) if(ents[i]->type == *type && k++ == *n)
        {
            player1->o = player1->newpos = ents[i]->o;
            player1->o.z += player1->eyeheight;
            player1->newpos.z += player1->eyeheight;
            player1->vel = vec(0, 0, 0);
            conoutf("harnessgotoent: type %d #%d at %.1f %.1f %.1f", *type, *n, ents[i]->o.x, ents[i]->o.y, ents[i]->o.z);
            return;
        }
        conoutf(CON_ERROR, "harnessgotoent: no entity #%d of type %d", *n, *type);
    });
    ICOMMAND(harnessguiclick, "s", (char *action), schedulemenuaction(action, true));
    ICOMMAND(harnessstall, "i", (int *ms), { if(*ms > 0) SDL_Delay(*ms); });

    // bots go into the player group; without bots the group hashes exactly as before
    static void hashbots(dethasher &h)
    {
        loopv(players)
        {
            fpsent *d = players[i];
            if(!d->ai) continue;
            h.i(d->clientnum);
            h.phys(d);
            h.i(d->health); h.i(d->armour); h.i(d->armourtype);
            h.i(d->gunselect); h.i(d->gunwait); h.i(d->quadmillis);
            loopk(NUMGUNS) h.i(d->ammo[k]);
            h.i(d->lastaction); h.i(d->lastpain); h.i(d->lifesequence); h.i(d->attacking);
            const ai::aiinfo &a = *d->ai;
            h.i(a.state.length());
            loopvk(a.state)
            {
                const ai::aistate &b = a.state[k];
                h.i(b.type); h.i(b.millis); h.i(b.targtype); h.i(b.target); h.i(b.idle); h.i(b.override);
            }
            h.i(a.route.length());
            loopvk(a.route) h.i(a.route[k]);
            h.v(a.target); h.v(a.spot);
            h.i(a.enemy); h.i(a.enemyseen); h.i(a.enemymillis); h.i(a.weappref);
            loopk(ai::NUMPREVNODES) h.i(a.prevnodes[k]);
            h.i(a.targnode); h.i(a.targlast); h.i(a.targtime); h.i(a.targseq);
            h.i(a.lastrun); h.i(a.lasthunt); h.i(a.lastaction); h.i(a.lastcheck);
            h.i(a.jumpseed); h.i(a.jumprand); h.i(a.blocktime); h.i(a.huntseq); h.i(a.blockseq); h.i(a.lastaimrnd);
            h.f(a.targyaw); h.f(a.targpitch);
            loopk(3) { h.f(a.views[k]); h.f(a.aimrnd[k]); }
            h.i(a.dontmove); h.i(a.becareful); h.i(a.tryreset); h.i(a.trywipe);
        }
    }

    VAR(dethashdetail, 0, 0, 1);

    // player, monsters, projectiles, movables, entities, rng, server, actors, world
    void hashgamestate(uint groups[DETHASH_GROUPS])
    {
        dethasher pl;
        pl.phys(player1);
        pl.i(player1->health); pl.i(player1->armour); pl.i(player1->armourtype);
        pl.i(player1->gunselect); pl.i(player1->gunwait); pl.i(player1->quadmillis);
        loopi(NUMGUNS) pl.i(player1->ammo[i]);
        pl.i(player1->lastaction); pl.i(player1->lastpain); pl.i(player1->lifesequence);
        pl.i(player1->lasttaunt); pl.i(player1->attacking);
        hashbots(pl);

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

        groups[0] = pl.h; groups[1] = mo.h; groups[2] = pr.h; groups[3] = mv.h; groups[4] = en.h;
        groups[5] = hashMT(2166136261U);

        dethasher sv, ac, wo;
        server::hashstate(sv);

        loopv(players)
        {
            fpsent *d = players[i];
            if(d != player1 && !d->ai) continue;
            ac.i(d->clientnum); ac.v(d->deltapos); ac.i(d->lastpickup); ac.i(d->lastpickupmillis);
            ac.i(d->flagpickup); ac.i(d->maxhealth); ac.i(d->vertical); ac.i(d->hovering);
            ac.i(d->respawned); ac.i(d->suicided);
        }
        hashshotids(ac);

        // with dethashdetail, each part of the world group is also hashed on its own and logged
        dethasher wents, wclock, wclient;
        uint wsleeps = 2166136261U, wphys = 2166136261U;
        // the renderer and the sound system set EF_RENDER and EF_SOUND
        loopv(ents) wents.i(ents[i]->flags & ~(EF_RENDER | EF_SOUND));
        hashgameclock(wclock);
        hashclientstate(wclient);
        hashsleeps(wsleeps);
        hashphysicsstate(wphys);
        wo.u(wents.h); wo.u(wclock.h); wo.u(wclient.h); wo.u(wsleeps); wo.u(wphys);
        if(dethashdetail) conoutf("dethashdetail tick %d world: ents %08x clock %08x client %08x sleeps %08x phys %08x", inputtick(), wents.h, wclock.h, wclient.h, wsleeps, wphys);

        groups[6] = sv.h; groups[7] = ac.h; groups[8] = wo.h;
    }

    void dethashframe()
    {
        if(!dethashfile) return;

        uint g[DETHASH_GROUPS];
        hashgamestate(g);
        dethashfile->printf("%d %d", dethashframes++, lastmillis);
        loopi(DETHASH_GROUPS) dethashfile->printf(" %08x", g[i]);
        dethashfile->printf("\n");

        if(dethashframes == 1 && dethashonstart[0]) execident(dethashonstart);
    }
}
