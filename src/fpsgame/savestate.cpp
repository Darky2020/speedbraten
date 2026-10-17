// Savestates: the whole offline simulation state written to a byte buffer at a tick
// boundary (the start of begintick) and restored from it bit-exactly. The buffer is
// portable between platforms. Cosmetic state (particles, gibs' sounds, ragdolls, render and
// sound entity flags) is not part of it.

#include "game.h"
#include "inputs.h"

namespace game
{
    enum { SNAPVERSION = 1 };
    static const uint SNAPMAGIC = 0x54534253U; // "SBST"
    static const int COSMETICENTFLAGS = EF_RENDER | EF_SOUND;

    static bool mapedited = false;

    void noteworldedit() { mapedited = true; }

    static uint snaptag(const char *s) { return uint(s[0]) | (uint(s[1])<<8) | (uint(s[2])<<16) | (uint(s[3])<<24); }

    void snapfpsent(snapio &s, fpsent &d)
    {
        s.phys(d);
        s.n(d.k_left); s.n(d.k_right); s.n(d.k_forward); s.n(d.k_backward); s.n(d.k_up); s.n(d.k_down);
        s.state(d);
        s.i(d.weight); s.i(d.lifesequence); s.i(d.respawned); s.i(d.suicided);
        s.i(d.lastpain); s.i(d.lastaction); s.i(d.lastattackgun); s.i(d.lasthitpushgun);
        s.n(d.attacking); s.i(d.lasttaunt);
        s.i(d.lastpickup); s.i(d.lastpickupmillis); s.i(d.lastbase); s.i(d.lastrepammo);
        s.i(d.flagpickup); s.i(d.tokens); s.v(d.lastcollect);
        s.i(d.frags); s.i(d.flags); s.i(d.deaths); s.i(d.totaldamage); s.i(d.totalshots); s.i(d.suicides);
        s.str(d.team, sizeof(d.team));
        s.i(d.lastnode);
    }

    static void snapentities(snapio &s)
    {
        const vector<extentity *> &ents = entities::getents();
        int n = s.count(ents.length(), ents.length());
        if(s.loading && n != ents.length()) s.ok = false;
        if(!s.ok) return;
        loopv(ents)
        {
            fpsentity &e = *(fpsentity *)ents[i];
            int type = e.type, flags = e.flags & ~COSMETICENTFLAGS;
            s.i(type);
            if(type != e.type) { s.ok = false; return; }
            s.i(flags);
            if(s.loading) e.flags = (e.flags & COSMETICENTFLAGS) | (flags & ~COSMETICENTFLAGS);
            s.i(e.triggerstate); s.i(e.lasttrigger);
        }
    }

    static void snaprng(snapio &s)
    {
        uint st[MTSTATESIZE];
        int cur = 0;
        if(!s.loading) saveMT(st, cur);
        loopi(MTSTATESIZE) s.u(st[i]);
        s.i(cur);
        if(s.loading && s.ok) loadMT(st, cur);
    }

    static void snapdynentcache(snapio &s)
    {
        vector<int> v;
        if(!s.loading && !savedynentcache(v)) v.setsize(0);
        int n = s.count(v.length(), 1<<22);
        if(s.loading) loopi(n) v.add(0);
        loopv(v) s.i(v[i]);
        if(s.loading && s.ok && !loaddynentcache(v)) s.ok = false;
    }

    static void snapscripts(snapio &s)
    {
        vector<char> b;
        if(!s.loading) savescriptstate(b);
        s.bytes(b);
        if(s.loading && s.ok && !loadscriptstate(b)) s.ok = false;
    }

    // Header fields that must match the running game; checked before any state is touched.
    static bool snapheader(snapio &s)
    {
        uint magic = SNAPMAGIC;
        int version = SNAPVERSION, mode = gamemode, ents = entities::getents().length();
        uint crc = getmapcrc();
        string map;
        copystring(map, getclientmap());
        s.u(magic); s.i(version); s.str(map, sizeof(map)); s.u(crc); s.i(mode); s.i(ents);
        if(!s.loading) return true;
        return s.ok && magic == SNAPMAGIC && version == SNAPVERSION && !strcmp(map, getclientmap()) &&
               crc == getmapcrc() && mode == gamemode && ents == entities::getents().length();
    }

    static void snapall(snapio &s)
    {
        // objects are recreated first, so that saved dynent indices resolve
        s.tag(snaptag("MONS")); snapmonsters(s);
        if(!s.ok) return;
        s.tag(snaptag("MONF")); snapmonsterfields(s);
        s.tag(snaptag("MOVE")); snapmovables(s);
        s.tag(snaptag("CLK ")); snapgameclock(s);
        s.tag(snaptag("RNG ")); snaprng(s);
        s.tag(snaptag("PLY ")); snapfpsent(s, *player1);
        s.tag(snaptag("ENT ")); snapentities(s);
        s.tag(snaptag("SHOT")); snapshots(s);
        s.tag(snaptag("SERV")); server::snapstate(s);
        s.tag(snaptag("MODE")); snapclientmodes(s);
        s.tag(snaptag("DYNC")); snapdynentcache(s);
        s.tag(snaptag("SCRP")); snapscripts(s);
        s.tag(snaptag("RUN ")); snapruntimer(s);
        s.tag(snaptag("REC ")); snaprecorder(s);
        s.tag(snaptag("END "));
    }

    static const char *snapshotrefusal()
    {
        if(!connected || multiplayer(false)) return "savestates only work offline";
        if(editmode) return "savestates do not work in edit mode";
        if(mapedited) return "savestates do not work once the map was edited";
        if(m_capture || m_collect || m_hold) return "savestates do not work in capture, collect or hold modes";
        return NULL;
    }

    static bool writesnapshot(vector<uchar> &buf)
    {
        buf.setsize(0);
        if(snapshotrefusal()) return false;
        snapio s(buf, false);
        snapheader(s);
        snapall(s);
        return true;
    }

    static bool readsnapshot(vector<uchar> &buf)
    {
        if(snapshotrefusal()) return false;
        snapio s(buf, true);
        if(!snapheader(s)) return false;
        snapall(s);
        if(!s.ok) conoutf(CON_ERROR, "savestate: the saved state is damaged; the game is now in an undefined state");
        return s.ok;
    }

    static uint bufdigest(const vector<uchar> &buf)
    {
        uint h = 2166136261U;
        loopv(buf) h = (h ^ buf[i]) * 16777619U;
        return h;
    }

    uint snapshotdigest()
    {
        vector<uchar> buf;
        return writesnapshot(buf) ? bufdigest(buf) : 0;
    }

    enum { SNAP_NONE = 0, SNAP_SAVE, SNAP_LOAD, SNAP_REPLAY };
    static vector<uchar> savedstate;
    static int pendingsnap = SNAP_NONE;

    static bool restoresavedstate() { return readsnapshot(savedstate); }
    static int cycleticks = 0, cyclemismatches = 0;

    void savestatemapstart()
    {
        mapedited = false;
        pendingsnap = SNAP_NONE;
    }

    // Harness check: save and restore the state at every tick. A run with snapcycle 1 must
    // hash exactly like the same run without it, and every restored state must save back to
    // the same bytes.
    VARF(snapcycle, 0, 0, 1, { cycleticks = cyclemismatches = 0; });

    static void cyclestate()
    {
        vector<uchar> a, b;
        if(!writesnapshot(a)) return;
        if(!readsnapshot(a)) { cyclemismatches++; return; }
        writesnapshot(b);
        cycleticks++;
        if(a.length() != b.length() || memcmp(a.getbuf(), b.getbuf(), a.length()))
        {
            if(!cyclemismatches) conoutf(CON_ERROR, "snapcycle: restored state saves differently at tick %d", inputtick());
            cyclemismatches++;
        }
    }

    void savestatetick()
    {
        int op = pendingsnap;
        pendingsnap = SNAP_NONE;
        if(op == SNAP_SAVE)
        {
            if(writesnapshot(savedstate)) conoutf("state saved at tick %d (%d bytes)", inputtick(), savedstate.length());
            else conoutf(CON_ERROR, "%s", snapshotrefusal());
        }
        else if(op == SNAP_LOAD)
        {
            if(savedstate.empty()) conoutf(CON_ERROR, "no saved state");
            else if(inputreplayactive()) conoutf(CON_ERROR, "savestates do not work during a replay");
            else if(readsnapshot(savedstate)) conoutf("state loaded (tick %d)", inputtick());
            else if(snapshotrefusal()) conoutf(CON_ERROR, "%s", snapshotrefusal());
            else conoutf(CON_ERROR, "the saved state belongs to another map or mode");
        }
        else if(op == SNAP_REPLAY)
        {
            if(savedstate.empty()) conoutf(CON_ERROR, "no saved state");
            else if(inputreplayfromstate(restoresavedstate)) conoutf("state loaded (tick %d), replaying the recorded input from there", inputtick());
            else conoutf(CON_ERROR, "could not replay from the saved state");
        }
        if(snapcycle) cyclestate();
    }

    ICOMMAND(savestate, "", (), pendingsnap = SNAP_SAVE);
    ICOMMAND(loadstate, "", (), pendingsnap = SNAP_LOAD);
    // harness: rewinds to the saved state and replays the current recording from it
    ICOMMAND(loadstatereplay, "", (), pendingsnap = SNAP_REPLAY);
    void snapcyclereport()
    {
        if(snapcycle) conoutf("snapcycle: %d ticks saved and restored, %d mismatches", cycleticks, cyclemismatches);
    }
    ICOMMAND(snapdigest, "", (),
    {
        defformatstring(res, "%08x", snapshotdigest());
        result(res);
    });
}
