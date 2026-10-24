// Spectating another player's witnessed run through the hub: the hub sends the run's replay
// file as its segments close (SPECRUN, SPECDATA), and the run is played back from that growing
// file as the witness's live re-simulation does it. Playback holds the simulation when it needs
// a tick the file doesn't have yet, and plays faster while it is far behind.

#include "game.h"
#include "inputs.h"

namespace game
{
    // views from the hub have segments of this many ticks in every mode
    enum { CATCHUPSPEED = 1000, VIEWSEGMENT = 200 };

    static uint spectarget = 0, specrun = 0;
    static bool specstarted = false, specchained = false;
    static int specbytes = 0, specparsed = 0, specknown = 0;
    static string specname = "";
    static string returnmap = "", returnkind = "";
    static int returnmode = 0;
    static const char *specfile = "spectate.sbr";

    bool spectating() { return spectarget != 0; }
    uint spectatingid() { return spectarget; }
    const char *spectatingname() { return specname; }

    // the simulation waits for the file to catch up with playback
    bool spectatehold() { return spectarget && specstarted && inputreplayneeds(specknown); }

    // percent of normal speed: fast while more than two segments behind
    int spectatespeed()
    {
        if(!spectarget || !specstarted) return 100;
        return specknown - inputtick() > 2*VIEWSEGMENT ? CATCHUPSPEED : 100;
    }

    // seconds of play the spectator is behind the newest segment, for the HUD
    float spectatebehind() { return spectarget && specstarted ? max(specknown - inputtick(), 0)*SIMTICK/1000.0f : 0; }

    void spectatebegin(uint target, const char *name, const char *map, int mode, const char *kind)
    {
        if(!spectarget)
        {
            copystring(returnmap, map);
            returnmode = mode;
            copystring(returnkind, kind);
        }
        spectarget = target;
        copystring(specname, name);
        specrun = 0;
        specstarted = false;
    }

    // a new run of the spectated player: the file starts over
    void spectaterun(uint run, const char *map, int mode, bool chained)
    {
        if(!spectarget) return;
        stream *f = openrawfile(path(specfile, true), "wb");
        if(f) delete f;
        specrun = run;
        specbytes = specparsed = specknown = 0;
        specchained = chained && specstarted && inputreplayactive();
        specstarted = false;

        conoutf("\f2spectating %s on %s", specname, map);
    }

    void spectatedata(uint run, int offset, const uchar *data, int len)
    {
        if(!spectarget || run != specrun || offset != specbytes) return;
        stream *f = openrawfile(path(specfile, true), "ab");
        if(!f) return;
        f->write(data, len);
        delete f;
        specbytes += len;
    }

    static void spectatefinish(bool reload)
    {
        spectarget = specrun = 0;
        specstarted = false;
        specname[0] = '\0';
        if(!reload || remote) return;
        // back to the player's own map, or a fresh game on the watched one
        if(!returnmap[0]) { copystring(returnmap, clientmap); returnmode = gamemode; returnkind[0] = '\0'; }
        if(!returnmap[0]) return;
        server::forcenextflagrun(!strcmp(returnkind, "flagrun") ? 1 : 0);
        server::forcenextracerules(!strcmp(returnkind, "race") ? 1 : 0);
        changemap(returnmap, returnmode);
    }

    void spectateend(const char *why)
    {
        if(!spectarget) return;
        conoutf("\f2stopped spectating: %s", why);
        spectatefinish(true);
    }

    void spectatestop() { if(spectarget) spectatefinish(true); }

    // once per frame: plays what has arrived
    void spectateupdate()
    {
        if(!spectarget || specbytes == specparsed) return;
        inputrun run;
        replayinfo info;
        int known = 0;
        if(!readreplayprefix(specfile, run, info, known)) return;
        specparsed = specbytes;
        specknown = known;
        if(specstarted) { inputreplayextend(run, false, false); return; }
        if(run.empty()) return;
        if(specchained && inputreplaycontinue(run)) {}
        else playinputrun(run, specfile);
        inputreplayextend(run, false, false);
        specstarted = true;
    }
}
