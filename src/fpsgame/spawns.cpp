// Spawn tools. Flagruns: the player can set their own spawn point after standing still for a
// second; it is saved per map and mode in spawns/<map>_<mode>.cfg and applied through a recorded
// command, so replays respawn at the same point. Races: racestart picks a fixed playerstart.

#include "game.h"
#include "inputs.h"

namespace game
{
    // the map's Nth playerstart (of the default spawn group) for race respawns, -1 random;
    // a replay-forced sim setting
    MOD(VARP, racestart, -1, -1, 255);

    static struct customspawn
    {
        bool valid;
        vec o;
        float yaw, pitch;
    } spawn = { false, vec(0, 0, 0), 0, 0 };

    // respawnnow: the life whose end was asked for, until the next life has started; -1 none
    static int respawnlife = -1;

    // setspawn arms a countdown; standing still on the ground until it ends sets the spawn
    enum { STILLMS = 1000 };
    static const float STILLDIST = 1;
    static vec armpos(0, 0, 0);
    static int armedat = -1;

    void snapspawns(snapio &s)
    {
        s.n(spawn.valid); s.v(spawn.o); s.f(spawn.yaw); s.f(spawn.pitch);
    }
    void resetspawns(snapio &s) { spawn.valid = false; }
    void snaprespawn(snapio &s) { s.i(respawnlife); }
    void resetrespawn(snapio &s) { respawnlife = -1; }

    static bool customspawnactive() { return spawn.valid && m_ctf && server::flagrunactive(); }

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

    // places d at a custom spawn or returns the entity to force (-1: none)
    bool pickcustomspawn(fpsent *d, int &forceent)
    {
        if(d != player1) return false;
        if(customspawnactive())
        {
            d->o = spawn.o;
            d->yaw = spawn.yaw;
            d->pitch = spawn.pitch;
            d->roll = 0;
            d->resetinterp();
            return true;
        }
        if(racestart >= 0 && server::racerulesactive()) forceent = nthplayerstart(racestart);
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

    static void spawnfile(char *dst, const char *map, int mode)
    {
        string m;
        copystring(m, map);
        for(char *c = m; *c; c++) if(*c == '/' || *c == '\\') *c = '_';
        nformatstring(dst, MAXSTRLEN, "spawns/%s_%d.cfg", m, mode);
    }

    static void queuespawn(const vec &o, float yaw, float pitch, bool place)
    {
        defformatstring(cmd, "setspawnat %.9g %.9g %.9g %.9g %.9g %d", o.x, o.y, o.z, yaw, pitch, place ? 1 : 0);
        queueinput(INPUT_COMMAND, 0, cmd);
    }

    // a recorded command: sets the custom spawn, and with place also moves player1 there
    ICOMMAND(setspawnat, "fffffi", (float *x, float *y, float *z, float *yaw, float *pitch, int *place),
    {
        spawn.valid = true;
        spawn.o = vec(*x, *y, *z);
        spawn.yaw = *yaw;
        spawn.pitch = *pitch;
        if(*place && player1->state == CS_ALIVE)
        {
            player1->o = spawn.o;
            player1->yaw = spawn.yaw;
            player1->pitch = spawn.pitch;
            player1->vel = player1->falling = vec(0, 0, 0);
            player1->resetinterp();
        }
    });
    ICOMMAND(clearspawnat, "", (), spawn.valid = false);

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
        queuespawn(player1->o, player1->yaw, player1->pitch, false);
        string file;
        spawnfile(file, getclientmap(), gamemode);
        stream *f = openutf8file(path(file, true), "w");
        if(f)
        {
            f->printf("%.9g %.9g %.9g %.9g %.9g\n", player1->o.x, player1->o.y, player1->o.z, player1->yaw, player1->pitch);
            delete f;
        }
        conoutf("spawn set for %s (%s)", getclientmap(), server::modename(gamemode, "?"));
    }

    ICOMMAND(clearspawn, "", (),
    {
        if(inputreplayactive()) return;
        queueinput(INPUT_COMMAND, 0, "clearspawnat");
        string file;
        spawnfile(file, getclientmap(), gamemode);
        stream *f = openutf8file(path(file, true), "w");
        delete f;
        conoutf("spawn cleared for %s", getclientmap());
    });

    ICOMMAND(havespawn, "", (), intret(customspawnactive() ? 1 : 0));
    bool spawnaidsactive(bool &custom, bool &race)
    {
        custom = customspawnactive();
        race = racestart >= 0 && server::racerulesactive();
        return custom || race;
    }
    ICOMMAND(racestartcount, "", (), intret(countplayerstarts()));
    ICOMMAND(inrace, "", (), intret(!multiplayer(false) && server::racerulesactive() ? 1 : 0));

    // a saved spawn of this map and mode is applied, and player1 placed there, on the first tick
    void spawnmapstart(const char *map)
    {
        spawn.valid = false;
        armedat = -1;
        if(multiplayer(false) || !m_ctf || !server::flagrunactive() || inputreplayactive()) return;
        string file;
        spawnfile(file, map, gamemode);
        stream *f = openutf8file(path(file, true), "r");
        if(!f) return;
        char line[256];
        vec o;
        float yaw, pitch;
        if(f->getline(line, sizeof(line)) && sscanf(line, "%f %f %f %f %f", &o.x, &o.y, &o.z, &yaw, &pitch) == 5) queuespawn(o, yaw, pitch, true);
        delete f;
    }
}
