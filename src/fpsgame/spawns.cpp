// Spawn tools. Flagruns: the player spawns at the playerstart nearest to the enemy flag, or at a
// spawn point of their own, set by standing still for a second; it is saved per map, mode and team
// in spawns/<map>_<mode>_<team>.cfg and applied through a recorded command, so replays respawn at
// the same point. Races: racestart picks a fixed playerstart.

#include "game.h"
#include "sbcompat.h"
#include "inputs.h"

namespace game
{
    // the map's Nth playerstart (of the default spawn group) for race respawns, -1 random;
    // a replay-forced sim setting
    MOD(VARP, racestart, -1, -1, 255);
    // flagruns without a custom spawn start at the playerstart nearest to the enemy flag rather than
    // at a random one of the team's; a replay-forced sim setting
    MOD(VAR, flagrunspawn, 0, 1, 1);

    struct customspawn
    {
        bool valid;
        vec o;
        float yaw, pitch;
    };
    // the custom spawns of the good and the evil team
    static customspawn spawns[2] = { { false, vec(0, 0, 0), 0, 0 }, { false, vec(0, 0, 0), 0, 0 } };
    static const char * const spawnteams[2] = { "good", "evil" };

    // 1 good, 2 evil, 0 neither
    static int teamnum(const char *team) { return !strcmp(team, "good") ? 1 : !strcmp(team, "evil") ? 2 : 0; }
    static customspawn *teamspawn() { int t = teamnum(player1->team); return t ? &spawns[t-1] : NULL; }

    // respawnnow: the life whose end was asked for, until the next life has started; -1 none
    static int respawnlife = -1;

    // setspawn arms a countdown; standing still on the ground until it ends sets the spawn
    enum { STILLMS = 1000 };
    static const float STILLDIST = 1;
    static vec armpos(0, 0, 0);
    static int armedat = -1;

    static void snapspawn(snapio &s, customspawn &c) { s.n(c.valid); s.v(c.o); s.f(c.yaw); s.f(c.pitch); }
    // states saved before spawns were per team hold one spawn, for both teams
    void snapspawns(snapio &s)
    {
        snapspawn(s, spawns[0]);
        if(s.loading) spawns[1] = spawns[0];
    }
    void resetspawns(snapio &s) { spawns[0].valid = spawns[1].valid = false; }
    void snapteamspawns(snapio &s) { snapspawn(s, spawns[0]); snapspawn(s, spawns[1]); }
    void resetteamspawns(snapio &s) {}
    void snaprespawn(snapio &s) { s.i(respawnlife); }
    void resetrespawn(snapio &s) { respawnlife = -1; }

    static bool customspawnactive() { customspawn *c = teamspawn(); return c && c->valid && m_ctf && server::flagrunactive(); }

    static int nthplayerstart(int n)
    {
        const vector<extentity *> &ents = entities::getents();
        loopv(ents) if(ents[i]->type == PLAYERSTART && !ents[i]->attr2 && !n--) return i;
        return -1;
    }

    static int countplayerstarts()
    {
        const vector<extentity *> &ents = entities::getents();
        int n = 0;
        loopv(ents) if(ents[i]->type == PLAYERSTART && !ents[i]->attr2) n++;
        return n;
    }

    // the playerstart nearest to a flag of the team other than player1's, -1 if none; ties go to
    // the first in the map's entity list
    static int nearestflagstart()
    {
        int team = teamnum(player1->team);
        if(!team) return -1;
        const vector<extentity *> &ents = entities::getents();
        int best = -1;
        float bestdist = 0;
        loopv(ents) if(ents[i]->type == PLAYERSTART)
        {
            loopvj(ents) if(ents[j]->type == FLAG && ents[j]->attr2 == 3 - team)
            {
                float dist = ents[i]->o.squaredist(ents[j]->o);
                if(best < 0 || dist < bestdist) { best = i; bestdist = dist; }
            }
        }
        return best;
    }

    // places d at a custom spawn or returns the entity to force (-1: none)
    bool pickcustomspawn(fpsent *d, int &forceent)
    {
        if(d != player1) return false;
        if(customspawnactive())
        {
            const customspawn &spawn = *teamspawn();
            d->o = spawn.o;
            d->yaw = spawn.yaw;
            d->pitch = spawn.pitch;
            d->roll = 0;
            d->resetinterp();
            return true;
        }
        if(racestart >= 0 && server::racerulesactive()) forceent = nthplayerstart(racestart);
        else if(flagrunspawn && m_ctf && server::flagrunactive()) forceent = nearestflagstart();
        return false;
    }

    // A suicide in a multiplayer mode reaches the server as an event, so a spawn request in
    // the same tick is refused; respawnnow keeps asking until the new life has started.
    ICOMMAND(respawnnow, "", (),
    {
        if(deferinput(INPUT_COMMAND, 0, "respawnnow")) return;
        if(!connected || intermission) return;
        respawnlife = player1->lifesequence;
        if(player1->state == CS_ALIVE) suicide(player1);
        respawn();
    });

    void spawntick()
    {
        if(respawnlife >= 0)
        {
            if(player1->state == CS_ALIVE && player1->lifesequence != respawnlife) respawnlife = -1;
            else if(player1->state == CS_DEAD) respawn();
        }
        if(armedat >= 0) checkarmedspawn();
    }

    static bool standing() { return player1->state == CS_ALIVE && player1->physstate >= PHYS_SLOPE && !player1->timeinair; }

    // a team's spawn file (team 1 good, 2 evil), or with team 0 the one saved before spawns were per team
    static void spawnfile(char *dst, const char *map, int mode, int team)
    {
        string m;
        copystring(m, map);
        for(char *c = m; *c; c++) if(*c == '/' || *c == '\\') *c = '_';
        if(team) nformatstring(dst, MAXSTRLEN, "spawns/%s_%d_%s.cfg", m, mode, spawnteams[team-1]);
        else nformatstring(dst, MAXSTRLEN, "spawns/%s_%d.cfg", m, mode);
    }

    static void queuespawn(const vec &o, float yaw, float pitch, bool place, int team)
    {
        defformatstring(cmd, "setspawnat %.9g %.9g %.9g %.9g %.9g %d %d", o.x, o.y, o.z, yaw, pitch, place ? 1 : 0, team);
        queueinput(INPUT_COMMAND, 0, cmd);
    }

    // A recorded command: sets team's custom spawn (0: both teams, as replays recorded before spawns
    // were per team mean it), and with place also moves player1 there when it is player1's team.
    ICOMMAND(setspawnat, "fffffii", (float *x, float *y, float *z, float *yaw, float *pitch, int *place, int *team),
    {
        loopi(2) if(!*team || *team == i+1)
        {
            spawns[i].valid = true;
            spawns[i].o = vec(*x, *y, *z);
            spawns[i].yaw = *yaw;
            spawns[i].pitch = *pitch;
        }
        if(*place && player1->state == CS_ALIVE && (!*team || *team == teamnum(player1->team)))
        {
            player1->o = vec(*x, *y, *z);
            player1->yaw = *yaw;
            player1->pitch = *pitch;
            player1->vel = player1->falling = vec(0, 0, 0);
            player1->resetinterp();
        }
    });
    ICOMMAND(clearspawnat, "i", (int *team), loopi(2) if(!*team || *team == i+1) spawns[i].valid = false);

    ICOMMAND(setspawn, "", (),
    {
        if(!m_ctf || !server::flagrunactive() || multiplayer(false)) { conoutf(CON_ERROR, "setspawn: only in flagruns"); return; }
        if(inputreplayactive()) { conoutf(CON_ERROR, "setspawn: not during a replay"); return; }
        if(!standing()) { conoutf(CON_ERROR, "setspawn: stand on the ground first"); return; }
        armedat = lastmillis;
        armpos = player1->o;
    });

    // the countdown under the crosshair while setspawn is armed
    bool spawncountdown(char *buf)
    {
        if(armedat < 0) return false;
        nformatstring(buf, MAXSTRLEN, "setting spawn in %.1f s", max(STILLMS - (lastmillis - armedat), 0)/1000.0f);
        return true;
    }

    // harness: the countdown text, empty when not armed
    ICOMMAND(spawncountdowntext, "", (), { string buf = ""; spawncountdown(buf); result(buf); });

    void checkarmedspawn()
    {
        if(!standing() || player1->o.dist(armpos) >= STILLDIST || !m_ctf || !server::flagrunactive() || inputreplayactive())
        {
            armedat = -1;
            conoutf(CON_ERROR, "spawn not set: you moved");
            return;
        }
        if(lastmillis - armedat < STILLMS) return;
        armedat = -1;
        int team = teamnum(player1->team);
        if(!team) { conoutf(CON_ERROR, "spawn not set: you are on no team"); return; }
        queuespawn(player1->o, player1->yaw, player1->pitch, false, team);
        string file;
        spawnfile(file, getclientmap(), gamemode, team);
        stream *f = openutf8file(path(file, true), "w");
        if(f)
        {
            f->printf("%.9g %.9g %.9g %.9g %.9g\n", player1->o.x, player1->o.y, player1->o.z, player1->yaw, player1->pitch);
            delete f;
        }
        conoutf("spawn set for %s (%s), team %s", getclientmap(), server::modename(gamemode, "?"), spawnteams[team-1]);
    }

    ICOMMAND(clearspawn, "", (),
    {
        if(inputreplayactive()) return;
        int team = teamnum(player1->team);
        if(!team) { conoutf(CON_ERROR, "clearspawn: you are on no team"); return; }
        defformatstring(cmd, "clearspawnat %d", team);
        queueinput(INPUT_COMMAND, 0, cmd);
        // an empty file, so the spawn saved before spawns were per team doesn't apply either
        string file;
        spawnfile(file, getclientmap(), gamemode, team);
        stream *f = openutf8file(path(file, true), "w");
        delete f;
        conoutf("spawn cleared for %s, team %s", getclientmap(), spawnteams[team-1]);
    });

    ICOMMAND(havespawn, "", (), intret(customspawnactive() ? 1 : 0));
    // harness: the playerstart a flagrun spawns at by default, and player1's team and position
    ICOMMAND(spawninfo, "", (),
    {
        int e = nearestflagstart();
        const vector<extentity *> &ents = entities::getents();
        vec start = ents.inrange(e) ? ents[e]->o : vec(-1, -1, -1);
        defformatstring(res, "start=%d at %.1f %.1f %.1f team=%s pos=%.1f %.1f %.1f custom=%d", e, start.x, start.y, start.z,
                        player1->team, player1->o.x, player1->o.y, player1->o.z, customspawnactive() ? 1 : 0);
        result(res);
    });
    bool spawnaidsactive(bool &custom, bool &race)
    {
        custom = customspawnactive();
        race = racestart >= 0 && server::racerulesactive();
        return custom || race;
    }
    ICOMMAND(racestartcount, "", (), intret(countplayerstarts()));
    ICOMMAND(inrace, "", (), intret(!multiplayer(false) && server::racerulesactive() ? 1 : 0));

    // Each team's saved spawn of this map and mode is applied on the first tick, and player1 placed
    // at their own team's (the team is not known yet here). A team without a file of its own takes
    // the spawn saved before spawns were per team.
    void spawnmapstart(const char *map)
    {
        spawns[0].valid = spawns[1].valid = false;
        armedat = -1;
        if(multiplayer(false) || !m_ctf || !server::flagrunactive() || inputreplayactive()) return;
        for(int team = 1; team <= 2; team++)
        {
            string file;
            spawnfile(file, map, gamemode, team);
            stream *f = openutf8file(path(file, true), "r");
            if(!f)
            {
                spawnfile(file, map, gamemode, 0);
                f = openutf8file(path(file, true), "r");
            }
            if(!f) continue;
            char line[256];
            vec o;
            float yaw, pitch;
            if(f->getline(line, sizeof(line)) && sscanf(line, "%f %f %f %f %f", &o.x, &o.y, &o.z, &yaw, &pitch) == 5) queuespawn(o, yaw, pitch, true, team);
            delete f;
        }
    }
}
