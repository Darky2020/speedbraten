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
    // A new view on the map being played that starts from a saved state replaces the playing
    // one in place once that one is played out, up to oldknown, without a map load.
    static bool specswitch = false;
    static int oldknown = 0;
    static string specmap = "", speckind = "";
    static int specmode = 0;
    static string specname = "";
    static string returnmap = "", returnkind = "";
    static int returnmode = 0;
    static const char *specfile = "spectate.sbr";

    bool spectating() { return spectarget != 0; }
    uint spectatingid() { return spectarget; }
    const char *spectatingname() { return specname; }

    // the simulation waits for the file to catch up with playback
    bool spectatehold() { return spectarget && specstarted && inputreplayneeds(specswitch ? oldknown : specknown); }

    // percent of normal speed: fast while the spectated run plays more than two segments behind
    int spectatespeed()
    {
        if(!spectarget || !specstarted || !inputreplayactive()) return 100;
        return (specswitch ? oldknown : specknown) - inputtick() > 2*VIEWSEGMENT ? CATCHUPSPEED : 100;
    }

    // seconds of play the spectator is behind the newest segment, for the HUD
    float spectatebehind() { return spectarget && specstarted ? max((specswitch ? oldknown : specknown) - inputtick(), 0)*SIMTICK/1000.0f : 0; }

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
    void spectaterun(uint run, const char *map, int mode, const char *kind, bool chained)
    {
        if(!spectarget) return;
        stream *f = openrawfile(path(specfile, true), "wb");
        if(f) delete f;
        bool same = specstarted && inputreplayactive() && !strcmp(map, specmap) && mode == specmode;
        if(same && !specswitch) oldknown = specknown;
        specswitch = same;
        specrun = run;
        specbytes = specparsed = specknown = 0;
        specchained = chained && specstarted && inputreplayactive();
        specstarted = specswitch;
        copystring(specmap, map);
        copystring(speckind, kind);
        specmode = mode;

        if(!specswitch) conoutf("\f2spectating %s on %s", specname, runlabel(map));
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
        specstarted = specswitch = false;
        specname[0] = '\0';
        // a fresh game on the map that was being watched, else back to the player's own map
        if(specmap[0]) { copystring(returnmap, specmap); returnmode = specmode; copystring(returnkind, speckind); }
        specmap[0] = '\0';
        if(!reload || remote) return;
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
    // stops without going back, for a map load that follows straight away
    void spectatequit() { if(spectarget) spectatefinish(false); }

    // harness: the spectate state
    ICOMMAND(spectatestate, "", (),
    {
        defformatstring(res, "target=%u started=%d switch=%d known=%d tick=%d replay=%d speed=%d map=%s", spectarget, specstarted ? 1 : 0,
                        specswitch ? 1 : 0, specswitch ? oldknown : specknown, inputtick(), inputreplayactive() ? 1 : 0, spectatespeed(), clientmap);
        result(res);
    });

    // once per frame: plays what has arrived
    void spectateupdate()
    {
        if(!spectarget || specbytes == specparsed) return;
        if(specswitch && !inputreplayneeds(oldknown)) return;
        inputrun run;
        replayinfo info;
        int known = 0;
        if(!readreplayprefix(specfile, run, info, known)) return;
        specparsed = specbytes;
        specknown = known;
        if(specswitch)
        {
            if(run.empty()) return;
            specswitch = false;
            if(inputreplayswitch(run)) { inputreplayextend(run, false, false); return; }
            specstarted = false;
        }
        else if(specstarted) { inputreplayextend(run, false, false); return; }
        if(run.empty()) return;
        if(specchained && inputreplaycontinue(run)) {}
        else playinputrun(run, specfile);
        inputreplayextend(run, false, false);
        specstarted = true;
    }
}
