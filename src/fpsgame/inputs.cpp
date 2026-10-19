// Offline input stream. Live input that arrives between ticks is queued and applied at the
// start of the next tick, and everything the simulation reads from the player is recorded
// per tick, so the current attempt can be replayed exactly.

#include "game.h"
#include "inputs.h"

extern int identflags;
extern int unescapestring(char *dst, const char *src, const char *end);
extern int zoom;

namespace game
{
    static vector<inputevent> pending;
    static inputrun recording, previous, replaydata;
    static int curtick = 0, replayattempt = 0, replaypos = 0, replaydesync = 0;
    static bool replaying = false, replaypending = false, havecontrols = false, recordingoff = false;
    static bool runsaved = false;
    static bool recordingran = false; // a flagrun/race life of the recording started its run
    static int lastactivetick = 0, resetarmtick = -1;
    static int replayedticks = 0;
    static bool replaystartstate = false;
    static int splicetextuntil = 0;
    static uint lastreplayhash[DETHASH_GROUPS];

    static void checkpendingreset();

    // movement, jump or attack input from the player (aim alone does not count)
    static void noteactivity()
    {
        if(player1->move || player1->strafe || player1->vertical || player1->jumping || player1->attacking) lastactivetick = curtick;
    }

    extern void saverunreplay(const inputrun &run, bool finished, int runtime, bool started);

    static const char * const simsettings[] = { "skill", "killsendsp", "slowmosp", "spawnwait", "attackspawn", "jumpspawn", "deadpush", "aiforcegun", "floatspeed", "racestart" };
    enum { NUMSIMSETTINGS = sizeof(simsettings)/sizeof(simsettings[0]) };
    // settings added after replays started recording settings (the last ones of simsettings),
    // with the value that matches replays and saved states made before them
    static const struct { const char *name; int absent; } latesettings[] = { { "racestart", -1 } };
    enum { NUMLATESETTINGS = sizeof(latesettings)/sizeof(latesettings[0]), NUMOLDSETTINGS = NUMSIMSETTINGS - NUMLATESETTINGS };
    static void readsettings(int *vals);

    // A saved state with the setting values in effect when it was taken, and the branch of the
    // recording it was taken on.
    struct statestart
    {
        vector<uchar> state;
        int tick, branch;
        int settings[NUMSIMSETTINGS];

        statestart() : tick(-1), branch(-1) {}
        bool valid() const { return tick >= 0; }
        void clear() { state.setsize(0); tick = branch = -1; }
    };

    // Segmented recordings form a tree: every live load starts a new branch at the loaded
    // state's tick, on the branch that state was taken on. A branch holds the input recorded
    // after its fork; the recording of the current attempt is always the current branch's
    // whole chain, so a state's history is its own chain however many loads came between.
    struct inputbranch
    {
        int parent, forktick, discarded;
        vector<inputevent> events;
        vector<tickhash> hashes;

        inputbranch(int parent, int forktick, int discarded) : parent(parent), forktick(forktick), discarded(discarded) {}
        ~inputbranch() { loopv(events) DELETEA(events[i].cmd); }
    };
    static vector<inputbranch *> branches;
    static int curbranch = -1;

    static void resetbranches()
    {
        branches.deletecontents();
        branches.add(new inputbranch(-1, 0, 0));
        curbranch = 0;
    }

    // true if tick of branch b lies on the chain that ends at tick headtick of branch head
    static bool onchain(int b, int tick, int head, int headtick)
    {
        for(int c = head, limit = headtick; c >= 0; limit = branches[c]->forktick, c = branches[c]->parent)
            if(c == b) return tick <= limit;
        return false;
    }

    // last tick shared by the chains that end at (a, atick) and (b, btick)
    static int commontick(int a, int atick, int b, int btick)
    {
        for(int c = a, limit = atick; c >= 0; limit = branches[c]->forktick, c = branches[c]->parent)
            for(int d = b, dlimit = btick; d >= 0; dlimit = branches[d]->forktick, d = branches[d]->parent)
                if(c == d) return min(limit, dlimit);
        return 0;
    }

    static void addchain(inputattempt &a, int b, int limit)
    {
        if(b < 0) return;
        const inputbranch &br = *branches[b];
        addchain(a, br.parent, br.forktick);
        loopv(br.events) if(br.events[i].tick <= limit)
        {
            inputevent &e = a.events.add(br.events[i]);
            if(e.cmd) e.cmd = newstring(e.cmd);
        }
        loopv(br.hashes) if(br.hashes[i].tick <= limit) a.hashes.add(br.hashes[i]);
        if(br.parent >= 0)
        {
            inputsplice &sp = a.splices.add();
            sp.tick = br.forktick;
            sp.discarded = br.discarded;
        }
    }

    // Moves the current branch's input out of the recording into the tree, starts a branch at
    // tick of branch from, and makes the recording that branch's chain.
    static void forkrecording(int from, int tick)
    {
        if(recording.attempts.empty() || curbranch < 0) return;
        inputattempt &rec = recording.last();
        inputbranch &cur = *branches[curbranch];
        loopv(rec.events) if(rec.events[i].tick > cur.forktick) { cur.events.add(rec.events[i]); rec.events[i].cmd = NULL; }
        loopv(rec.hashes) if(rec.hashes[i].tick > cur.forktick) cur.hashes.add(rec.hashes[i]);
        int discarded = rec.ticks - commontick(curbranch, rec.ticks, from, tick);
        curbranch = branches.length();
        branches.add(new inputbranch(from, tick, max(discarded, 0)));
        loopv(rec.events) DELETEA(rec.events[i].cmd);
        rec.events.setsize(0);
        rec.hashes.setsize(0);
        rec.splices.setsize(0);
        addchain(rec, curbranch, tick);
        rec.ticks = tick;
    }

    // The attempt from a saved state on: the state, then the events and hashes after its tick.
    static void cutattempt(inputattempt &a, const inputattempt &src, const statestart &from)
    {
        src.copyto(a);
        a.snapshot = from.state;
        a.starttick = from.tick;
        loopv(a.settings) loopj(NUMSIMSETTINGS) if(!strcmp(a.settings[i].name, simsettings[j])) a.settings[i].value = from.settings[j];
        int n = 0;
        loopv(a.events) if(a.events[i].tick <= from.tick) { DELETEA(a.events[i].cmd); n++; } else break;
        a.events.remove(0, n);
        n = 0;
        loopv(a.hashes) if(a.hashes[i].tick <= from.tick) n++; else break;
        a.hashes.remove(0, n);
        n = 0;
        loopv(a.splices) if(a.splices[i].tick <= from.tick) n++; else break;
        a.splices.remove(0, n);
    }

    // Flagruns and races are judged per life: the replay of a finished run starts at the
    // spawn of the life it was run in, and an unfinished life whose run started is saved
    // on its own when it ends.
    MOD(VARP, replaylifeonly, 0, 1, 1);
    static statestart lifestart, savedstart;
    static bool lifepending = false, lifesaved = false;

    // Automatic states for segmented runs, taken every autostateinterval ms while recording;
    // loadautostate steps back through them.
    MOD(VARP, autostateinterval, 0, 1000, 60000);
    MOD(VARP, autostatecount, 0, 30, 1000);
    static vector<statestart *> autostates;
    static bool autoloadpending = false;

    // After a state is loaded live, the simulation holds for loadstatedelay ms (wall clock) so
    // the player can react; frames, aim and further loads go on meanwhile.
    MOD(VARP, loadstatedelay, 0, 1000, 10000);
    static int holduntil = 0;

    void holdafterload()
    {
        if(replaying || loadstatedelay <= 0) return;
        holduntil = totalmillis + loadstatedelay;
        conoutf("resuming in %.1f s", loadstatedelay/1000.0f);
    }

    bool holdsim()
    {
        if(!holduntil) return false;
        if(totalmillis - holduntil < 0) return true;
        holduntil = 0;
        return false;
    }

    // A live load keeps the keys the player holds now: the state's movement and attack belong
    // to the moment it was saved, and the keys pressed then are not held any more.
    static bool loadlivestate(statestart &st)
    {
        fpsent &d = *player1;
        bool keys[6] = { d.k_left, d.k_right, d.k_forward, d.k_backward, d.k_up, d.k_down };
        schar move = d.move, strafe = d.strafe, vertical = d.vertical;
        bool jumping = d.jumping, attacking = d.attacking;
        if(!restorestate(st.state)) return false;
        if(!replaying && !recordingoff) forkrecording(st.branch, st.tick);
        if(lifestart.valid() && !onchain(lifestart.branch, lifestart.tick, st.branch, st.tick)) lifestart.clear();
        while(autostates.length() && autostates.last()->tick > st.tick) delete autostates.pop();
        d.k_left = keys[0]; d.k_right = keys[1]; d.k_forward = keys[2]; d.k_backward = keys[3]; d.k_up = keys[4]; d.k_down = keys[5];
        d.move = move; d.strafe = strafe; d.vertical = vertical;
        d.jumping = jumping; d.attacking = attacking;
        return true;
    }

    static void loadautostate();

    bool loadsavedstate() { return savedstart.valid() && loadlivestate(savedstart); }

    // the save and load requests of savestate, loadstate and loadautostate, at a tick boundary
    void runstateops()
    {
        if(!connected) return;
        if(savestatetick())
        {
            int tick = 0;
            savedstart.state = savedstatedata(tick);
            savedstart.tick = tick;
            savedstart.branch = curbranch;
            readsettings(savedstart.settings);
        }
        if(autoloadpending) { autoloadpending = false; loadautostate(); }
    }

    static bool havebots()
    {
        loopv(players) if(players[i]->ai) return true;
        return false;
    }

    static void takelifestate()
    {
        lifepending = false;
        lifestart.clear();
        if(!replaylifeonly || havebots() || !takesnapshot(lifestart.state)) { lifestart.clear(); return; }
        lifestart.tick = curtick;
        lifestart.branch = curbranch;
        readsettings(lifestart.settings);
    }

    static bool saveliferun(bool finished, int runtime)
    {
        if(!replaylifeonly || !lifestart.valid() || lifesaved || recording.attempts.length() != 1) return false;
        if(!finished && !runtimerliferan()) return true;
        inputrun run;
        cutattempt(run.add(), recording.last(), lifestart);
        saverunreplay(run, finished, runtime, true);
        lifesaved = true;
        return true;
    }

    void inputlifespawned()
    {
        if(runtimerliferan()) recordingran = true;
        if(!replaying && !recordingoff) saveliferun(false, 0);
        lifestart.clear();
        lifesaved = false;
        lifepending = true;
    }

    static void takeautostate()
    {
        if(autostateinterval <= 0 || autostatecount <= 0 || replaying || recordingoff || curtick <= 0) return;
        if(curtick % max(autostateinterval / SIMTICK, 1)) return;
        statestart *st = new statestart;
        if(!takesnapshot(st->state)) { delete st; return; }
        st->tick = curtick;
        st->branch = curbranch;
        readsettings(st->settings);
        autostates.add(st);
        while(autostates.length() > autostatecount) delete autostates.remove(0);
    }

    // loads the newest automatic state that lies at least a quarter interval back
    static void loadautostate()
    {
        if(replaying || replaypending) { conoutf(CON_ERROR, "savestates do not work during a replay"); return; }
        int margin = max(autostateinterval / SIMTICK / 4, 1);
        for(int i = autostates.length()-1; i >= 0; i--) if(autostates[i]->tick <= curtick - margin)
        {
            int back = curtick - autostates[i]->tick;
            if(loadlivestate(*autostates[i]))
            {
                conoutf("auto state loaded (%.1f s back)", back*SIMTICK/1000.0f);
                holdafterload();
            }
            return;
        }
        conoutf(CON_ERROR, "no earlier auto state");
    }
    ICOMMAND(loadautostate, "", (), { autoloadpending = true; if(!insimtick) runstateops(); });

    static bool recordingstarted()
    {
        const inputattempt &first = *recording.attempts[0];
        if(!first.flagrun && !first.race) return m_check(first.mode, M_DMSP | M_CLASSICSP);
        return recordingran || runtimerliferan();
    }

    // Writes the current run as unfinished, once, unless it is a replay or was saved finished;
    // per life, the current life if its run started.
    static void saveunfinishedrun()
    {
        if(replaying || recordingoff || recording.empty()) return;
        if(saveliferun(false, 0)) { runsaved = true; return; }
        if(runsaved) return;
        saverunreplay(recording, false, 0, recordingstarted());
        runsaved = true;
    }

    void inputrunfinished(int runtime)
    {
        if(replaying || recordingoff || recording.empty()) return;
        if(!saveliferun(true, runtime)) saverunreplay(recording, true, runtime, true);
        runsaved = true;
    }

    static void restoresettings();

    void inputdisconnect()
    {
        saveunfinishedrun();
        restoresettings();
    }
    static inputcontrols lastcontrols;
    static vector<char> lastgui;
    static uint lastcursorx = 0, lastcursory = 0;
    static bool haveguistate = false;
    static int lastzoom = 0;

    static uint floatbits(float f) { uint u; memcpy(&u, &f, sizeof(u)); return u; }
    static float bitsfloat(uint u) { float f; memcpy(&f, &u, sizeof(f)); return f; }

    static void getcontrols(inputcontrols &c)
    {
        c.yaw = floatbits(player1->yaw);
        c.pitch = floatbits(player1->pitch);
        c.move = player1->move;
        c.strafe = player1->strafe;
        c.vertical = player1->vertical;
        c.jumping = player1->jumping;
        c.attacking = player1->attacking;
    }

    static void setcontrols(const inputcontrols &c)
    {
        player1->yaw = bitsfloat(c.yaw);
        player1->pitch = bitsfloat(c.pitch);
        player1->move = c.move;
        player1->strafe = c.strafe;
        player1->vertical = c.vertical;
        player1->jumping = c.jumping;
        player1->attacking = c.attacking;
    }

    // Live keys are ignored while a replay plays, so when it ends the replay's last controls
    // would otherwise keep the player moving or shooting.
    static void releasecontrols()
    {
        fpsent &d = *player1;
        d.k_left = d.k_right = d.k_forward = d.k_backward = d.k_up = d.k_down = false;
        d.move = d.strafe = d.vertical = 0;
        d.jumping = d.attacking = false;
    }

    static void clearevents(vector<inputevent> &evs)
    {
        loopv(evs) DELETEA(evs[i].cmd);
        evs.setsize(0);
    }

    void queueinput(int type, int arg, const char *cmd)
    {
        inputevent &e = pending.add();
        e.tick = 0;
        e.type = type;
        e.arg = arg;
        e.cursorx = e.cursory = 0;
        e.cmd = cmd ? newstring(cmd) : NULL;
    }

    // Live input is ignored while a replay or the determinism harness drives the player.
    bool inputlocked() { return (replaying || dethashactive()) && !insimtick; }

    // Returns true if the caller must not act now: offline input outside a tick is queued
    // for the next tick, or dropped while input is locked.
    bool deferinput(int type, int arg, const char *cmd)
    {
        if(!simticking || insimtick) return false;
        if(!inputlocked()) queueinput(type, arg, cmd);
        return true;
    }

    int inputtick() { return curtick; }

    // A map change started by the map's own script during play continues a singleplayer run.
    bool mapchained(int mode)
    {
        return insimtick && (identflags&IDF_OVERRIDDEN) && m_check(mode, M_DMSP | M_CLASSICSP);
    }

    static inputattempt &replaycur() { return *replaydata.attempts[replayattempt]; }

    // Settings the simulation reads that are not part of the player's input. A recording
    // stores them at the start of each attempt and records every later change; a replay
    // plays with the recorded values and then gives the player's own values back.
    static int cursettings[NUMSIMSETTINGS], usersettings[NUMSIMSETTINGS];
    static bool settingsforced = false;

    static int findsetting(const char *name)
    {
        loopi(NUMSIMSETTINGS) if(!strcmp(simsettings[i], name)) return i;
        return -1;
    }

    static int getsetting(int i)
    {
        ident *id = getident(simsettings[i]);
        return id && id->type == ID_VAR ? *id->storage.i : 0;
    }

    // writes the value directly, so a map script's override of the variable is kept
    static void setsetting(int i, int val)
    {
        ident *id = getident(simsettings[i]);
        if(!id || id->type != ID_VAR) return;
        val = clamp(val, id->minval, id->maxval);
        if(*id->storage.i == val) return;
        *id->storage.i = val;
        id->changed();
    }

    static void readsettings(int *vals) { loopi(NUMSIMSETTINGS) vals[i] = getsetting(i); }

    static void applysettings() { loopi(NUMSIMSETTINGS) setsetting(i, cursettings[i]); }

    static void forcesettings(const inputattempt &a)
    {
        if(!settingsforced)
        {
            readsettings(usersettings);
            settingsforced = true;
        }
        readsettings(cursettings);
        for(size_t j = 0; j < sizeof(latesettings)/sizeof(latesettings[0]); j++)
        {
            int k = findsetting(latesettings[j].name);
            if(k >= 0) cursettings[k] = latesettings[j].absent;
        }
        loopv(a.settings)
        {
            int k = findsetting(a.settings[i].name);
            if(k >= 0) cursettings[k] = a.settings[i].value;
        }
        applysettings();
    }

    static void restoresettings()
    {
        if(!settingsforced) return;
        settingsforced = false;
        loopi(NUMSIMSETTINGS) setsetting(i, usersettings[i]);
    }

    static void recordsettings(inputattempt &a)
    {
        readsettings(cursettings);
        a.settings.setsize(0);
        loopi(NUMSIMSETTINGS)
        {
            inputsetting &s = a.settings.add();
            copystring(s.name, simsettings[i]);
            s.value = cursettings[i];
        }
    }

    // records the settings changed since the last tick, other than through recorded input
    static void recordsettingchanges(inputattempt &a)
    {
        loopi(NUMSIMSETTINGS)
        {
            int val = getsetting(i);
            if(val == cursettings[i]) continue;
            cursettings[i] = val;
            inputevent &e = a.events.add();
            e.tick = curtick;
            e.type = INPUT_SETTING;
            e.arg = val;
            memset(&e.controls, 0, sizeof(e.controls));
            e.cursorx = e.cursory = 0;
            e.cmd = newstring(simsettings[i]);
        }
    }

    bool queuemenuinput(const char *cmd, bool scripted)
    {
        if(!simticking || insimtick || !connected) return false;
        if(scripted) queueinput(INPUT_COMMAND, 0, cmd);
        else deferinput(INPUT_COMMAND, 0, cmd);
        return true;
    }

    static void applyevent(const inputevent &e)
    {
        switch(e.type)
        {
            case INPUT_CONTROLS: lastcontrols = e.controls; break;
            case INPUT_JUMP: if(canjump()) player1->jumping = true; break;
            case INPUT_ATTACK: doattack(e.arg != 0); break;
            case INPUT_COMMAND: if(e.cmd) execute(e.cmd); break;
            case INPUT_GUI: setmapguistate(e.cmd ? e.cmd : "", bitsfloat(e.cursorx), bitsfloat(e.cursory)); break;
            case INPUT_ZOOM: zoom = e.arg; break;
            case INPUT_SETTING:
            {
                int i = e.cmd ? findsetting(e.cmd) : -1;
                if(i >= 0 && settingsforced) { cursettings[i] = e.arg; setsetting(i, e.arg); }
                break;
            }
        }
    }

    static const char * const hashgroupnames[DETHASH_GROUPS] = { "player", "monsters", "projectiles", "movables", "entities", "rng", "server", "actors", "world" };

    static int replaydesyncgroups = 0, replayhashpos = 0;

    // Reports the first mismatching tick, then logs when each further group first differs.
    static void checkreplayhash(const tickhash &h)
    {
        const inputattempt &a = replaycur();
        while(replayhashpos < a.hashes.length() && a.hashes[replayhashpos].tick < curtick) replayhashpos++;
        if(replayhashpos >= a.hashes.length() || a.hashes[replayhashpos].tick != curtick) return;
        const tickhash &want = a.hashes[replayhashpos];
        vector<char> diff;
        loopi(a.hashgroups) if(h.groups[i] != want.groups[i] && !(replaydesyncgroups & (1<<i)))
        {
            replaydesyncgroups |= 1<<i;
            if(diff.length()) diff.add(' ');
            diff.put(hashgroupnames[i], strlen(hashgroupnames[i]));
        }
        if(diff.empty()) return;
        diff.add('\0');
        if(!replaydesync)
        {
            replaydesync = curtick;
            conoutf(CON_ERROR, "replay desync at tick %d (%.3f s): %s", curtick, (curtick-1)*SIMTICK/1000.0f, diff.getbuf());
        }
        else logoutf("replay desync spreads at tick %d (%.3f s): %s", curtick, (curtick-1)*SIMTICK/1000.0f, diff.getbuf());
    }

    // On load during a recording, the attempt is cut back to the restored tick, so it still
    // replays from map start: the restored state is the state it had at that tick.
    void snaplatesettings(snapio &s) { for(int i = NUMOLDSETTINGS; i < NUMSIMSETTINGS; i++) s.i(cursettings[i]); }
    void resetlatesettings(snapio &s) { loopi(NUMLATESETTINGS) cursettings[NUMOLDSETTINGS + i] = latesettings[i].absent; }

    void snaprecorder(snapio &s)
    {
        int tick = curtick;
        s.i(tick);
        s.u(lastcontrols.yaw); s.u(lastcontrols.pitch);
        s.n(lastcontrols.move); s.n(lastcontrols.strafe); s.n(lastcontrols.vertical);
        s.n(lastcontrols.jumping); s.n(lastcontrols.attacking);
        s.n(havecontrols);
        s.bytes(lastgui);
        s.u(lastcursorx); s.u(lastcursory); s.n(haveguistate);
        s.i(lastzoom); s.i(zoom);
        loopi(NUMOLDSETTINGS) s.i(cursettings[i]);
        s.i(lastactivetick);
        if(!s.loading || !s.ok) return;
        curtick = tick;
        resetarmtick = -1;
    }

    // Restores a state saved earlier in the current recording, or in the replay that is
    // playing, and replays the recorded input from the restored tick, checking the state
    // hashes against the recording.
    bool inputreplayfromstate(bool (*restore)())
    {
        if(replaypending) return false;
        if(!replaying)
        {
            if(recordingoff || recording.attempts.empty()) return false;
            recording.copyto(replaydata);
            if(!restore()) { replaydata.clear(); return false; }
            replaying = true;
            replayattempt = replaydata.attempts.length()-1;
        }
        else if(!restore()) return false;
        const inputattempt &a = replaycur();
        replaypos = 0;
        while(replaypos < a.events.length() && a.events[replaypos].tick <= curtick) replaypos++;
        replayhashpos = 0;
        replaydesync = replaydesyncgroups = 0;
        replayedticks = 0;
        memset(lastreplayhash, 0, sizeof(lastreplayhash));
        return true;
    }

    void begintick()
    {
        if(!connected) { clearevents(pending); return; }
        if(replaystartstate && replaying)
        {
            replaystartstate = false;
            inputattempt &a = replaycur();
            if(!restorereplaystart(a.snapshot))
            {
                conoutf(CON_ERROR, "the replay's saved state does not fit this map or build: replay stopped");
                replaying = false;
                recordingoff = true;
                restoresettings();
            }
            else
            {
                replaypos = 0;
                while(replaypos < a.events.length() && a.events[replaypos].tick <= curtick) replaypos++;
                replayhashpos = 0;
            }
        }
        replaystartstate = false;
        if(lifepending) takelifestate();
        runstateops();
        snapcycletick();
        takeautostate();
        curtick++;
        tickhash h;
        if(replaying && curtick > replaycur().ticks)
        {
            replaying = false;
            recordingoff = true;
            restoresettings();
            releasecontrols();
            if(replaydesync) conoutf(CON_ERROR, "input replay finished (desynced at tick %d)", replaydesync);
            else conoutf("input replay finished");
        }
        noteactivity();
        checkpendingreset();
        if(!curtick) return;
        if(recordingoff)
        {
            // after a replay the game goes on live, but the attempt is not recorded
            while(pending.length())
            {
                inputevent e = pending.remove(0);
                applyevent(e);
                DELETEA(e.cmd);
                if(!recordingoff) return;
            }
            return;
        }
        if(replaying)
        {
            inputattempt &a = replaycur();
            if(curtick == 1 && a.mapcrc && getmapcrc() != a.mapcrc)
                conoutf(CON_ERROR, "replay was recorded on a different version of %s (map crc %08x, loaded %08x): it will not play back correctly", a.map, a.mapcrc, getmapcrc());
            if(curtick == 1 && a.hascfgcrc && getmapcfgcrc() != a.cfgcrc)
                conoutf(CON_ERROR, "replay was recorded with a different %s.cfg (crc %08x, loaded %08x): it will likely not play back correctly", a.map, a.cfgcrc, getmapcfgcrc());
            // mirrors the recording: setting changes come first in a tick, and changes made
            // by the tick's own input are not events
            while(replaypos < a.events.length() && a.events[replaypos].tick <= curtick && a.events[replaypos].type == INPUT_SETTING)
                applyevent(a.events[replaypos++]);
            if(settingsforced) applysettings();
            while(replaypos < a.events.length() && a.events[replaypos].tick <= curtick)
            {
                int attempt = replayattempt;
                applyevent(a.events[replaypos++]);
                if(!replaying || attempt != replayattempt || !curtick) return;
            }
            if(settingsforced) readsettings(cursettings);
            setcontrols(lastcontrols);
            // a splice at tick T: the player loaded the state of T, so T+1 is the first tick after
            int loads = 0;
            loopv(a.splices) if(a.splices[i].tick == curtick-1) loads++;
            if(loads)
            {
                if(loads > 1) conoutf("\f6splice at %.3f s: a state was loaded here (%d times)", (curtick-1)*SIMTICK/1000.0f, loads);
                else conoutf("\f6splice at %.3f s: a state was loaded here", (curtick-1)*SIMTICK/1000.0f);
                splicetextuntil = totalmillis + 1500;
            }
            hashgamestate(h.groups);
            checkreplayhash(h);
            memcpy(lastreplayhash, h.groups, sizeof(lastreplayhash));
            replayedticks++;
            return;
        }
        inputattempt &rec = recording.last();
        if(curtick == 1)
        {
            copystring(rec.team, player1->team);
            rec.mapcrc = getmapcrc();
            rec.cfgcrc = getmapcfgcrc();
            rec.hascfgcrc = true;
        }
        recordsettingchanges(rec);
        while(pending.length())
        {
            inputevent e = pending.remove(0);
            e.tick = curtick;
            applyevent(e);
            if(curtick != e.tick) { DELETEA(e.cmd); return; }
            rec.events.add(e);
        }
        readsettings(cursettings);
        inputcontrols c;
        getcontrols(c);
        if(!havecontrols || c != lastcontrols)
        {
            inputevent &e = rec.events.add();
            e.tick = curtick;
            e.type = INPUT_CONTROLS;
            e.arg = 0;
            e.controls = c;
            e.cursorx = e.cursory = 0;
            e.cmd = NULL;
            lastcontrols = c;
            havecontrols = true;
        }
        vector<char> gui;
        float cx, cy;
        getmapguistate(gui, cx, cy);
        if(!haveguistate || strcmp(gui.getbuf(), lastgui.getbuf()) || floatbits(cx) != lastcursorx || floatbits(cy) != lastcursory)
        {
            inputevent &e = rec.events.add();
            e.tick = curtick;
            e.type = INPUT_GUI;
            e.arg = 0;
            memset(&e.controls, 0, sizeof(e.controls));
            e.cursorx = lastcursorx = floatbits(cx);
            e.cursory = lastcursory = floatbits(cy);
            e.cmd = newstring(gui.getbuf());
            lastgui = gui;
            haveguistate = true;
        }
        // zooming out ends by itself (-1 -> 0), which the replay does too
        if(zoom != lastzoom && !(lastzoom < 0 && !zoom))
        {
            inputevent &e = rec.events.add();
            e.tick = curtick;
            e.type = INPUT_ZOOM;
            e.arg = zoom;
            memset(&e.controls, 0, sizeof(e.controls));
            e.cursorx = e.cursory = 0;
            e.cmd = NULL;
        }
        lastzoom = zoom;
        hashgamestate(h.groups);
        h.tick = curtick;
        rec.hashes.add(h);
        rec.ticks = curtick;
    }

    static void seedattempt(const inputattempt &a)
    {
        gameseed = a.seed;
        seedMT(gameseed);
        logoutf("game seed: %u (replay of %s, %d events)", gameseed, a.map, a.events.length());
    }

    void inputmapstart(const char *name, int mode)
    {
        bool chained = mapchained(mode) && !recordingoff;
        if(!chained || replaypending) saveunfinishedrun();
        lifestart.clear();
        lifepending = lifesaved = false;
        autostates.deletecontents();
        autoloadpending = false;
        savedstart.clear();
        resetbranches();
        clearevents(pending);
        curtick = 0;
        havecontrols = haveguistate = recordingoff = false;
        lastzoom = 0;
        lastactivetick = 0;
        resetarmtick = -1;
        replaypos = replayhashpos = 0;
        if(replaypending)
        {
            replaypending = false;
            replaying = true;
            replayattempt = 0;
            replaydesync = replaydesyncgroups = 0;
            seedattempt(replaycur());
            restoresettings();
            forcesettings(replaycur());
            replaystartstate = !replaycur().snapshot.empty();
            return;
        }
        if(replaying)
        {
            if(chained && replayattempt+1 < replaydata.attempts.length() && !strcmp(replaydata.attempts[replayattempt+1]->map, name))
            {
                replayattempt++;
                seedattempt(replaycur());
                forcesettings(replaycur());
                replaystartstate = !replaycur().snapshot.empty();
                return;
            }
            replaying = false;
            restoresettings();
            releasecontrols();
            conoutf(replaydesync ? CON_ERROR : CON_INFO, "input replay ended%s", replaydesync ? " (desynced)" : "");
            chained = false;
        }
        seedgame();
        if(!chained && !recording.empty()) recording.moveto(previous);
        if(!chained) { recording.clear(); runsaved = false; recordingran = false; }
        inputattempt &a = recording.add();
        a.seed = gameseed;
        copystring(a.map, name);
        a.mode = mode;
        a.flagrun = server::flagrunactive() ? 1 : 0;
        a.race = server::racerulesactive() ? 1 : 0;
        recordsettings(a);
    }

    void playinputrun(const inputrun &src)
    {
        src.copyto(replaydata);
        const inputattempt &first = *replaydata.attempts[0];
        server::forcenextteam(first.team);
        server::forcenextflagrun(first.flagrun);
        server::forcenextracerules(first.race);
        replaypending = true;
        replayedticks = 0;
        memset(lastreplayhash, 0, sizeof(lastreplayhash));
        changemap(first.map, first.mode);
    }

    bool inputreplayactive() { return replaying || replaypending; }
    bool inputrecordinglive() { return !replaying && !replaypending && !recordingoff; }
    int inputattemptindex() { return replaying ? replayattempt : max(recording.attempts.length()-1, 0); }

    // the first attempt of the run being played or replayed (its map and mode name the run)
    const inputattempt *inputrunfirst()
    {
        const inputrun &run = replaying ? replaydata : recording;
        return run.attempts.length() ? run.attempts[0] : NULL;
    }

    // state loads in the run being played: for a flagrun or race life saved on its own, only
    // those since the life started, as in its replay
    int inputsplicecount()
    {
        const inputrun &run = replaying ? replaydata : recording;
        int n = 0;
        if(!replaying && replaylifeonly && lifestart.valid() && run.attempts.length() == 1)
        {
            loopv(run.attempts[0]->splices) if(run.attempts[0]->splices[i].tick > lifestart.tick) n++;
            return n;
        }
        loopv(run.attempts) n += run.attempts[i]->splices.length();
        return n;
    }

    // first desync tick (0 = none), ticks replayed so far and the state hash after the last one
    int inputreplayresult(int &ticks, uint hash[DETHASH_GROUPS])
    {
        ticks = replayedticks;
        memcpy(hash, lastreplayhash, sizeof(lastreplayhash));
        return replaydesync;
    }

    MOD(VARP, speedrunresetidle, 0, 1000, 10000);

    // Pressing speedrunreset arms a restart of the current run from its first map; it fires
    // once the player has given no movement or attack input for speedrunresetidle ms after
    // the press, and any such input before that cancels it. A stray press mid-run therefore
    // never throws the run away.
    static void checkpendingreset()
    {
        if(resetarmtick < 0) return;
        if(lastactivetick > resetarmtick)
        {
            resetarmtick = -1;
            conoutf("\f3reset cancelled");
            return;
        }
        if((curtick - resetarmtick)*SIMTICK < speedrunresetidle) return;
        resetarmtick = -1;
        const inputrun &run = replaying ? replaydata : recording;
        if(run.attempts.length()) changemap(run.attempts[0]->map, run.attempts[0]->mode);
        else changemap(getclientmap(), gamemode);
    }

    ICOMMAND(speedrunreset, "", (),
    {
        if(!connected || multiplayer(false)) { conoutf(CON_ERROR, "speedrunreset only works offline"); return; }
        if(resetarmtick >= 0) return;
        resetarmtick = curtick;
        if(speedrunresetidle > 0) conoutf("\f2resetting in %.1f s - move or shoot to cancel", speedrunresetidle/1000.0f);
    });

    // small text under the crosshair: the hold after a load, an armed run reset, a splice in a
    // replay
    void drawinputtimers(int w, int h)
    {
        string buf = "";
        if(holduntil && totalmillis - holduntil < 0) formatstring(buf, "resuming in %.1f s", (holduntil - totalmillis)/1000.0f);
        else if(resetarmtick >= 0 && speedrunresetidle > 0)
            formatstring(buf, "resetting in %.1f s", max(speedrunresetidle - (curtick - resetarmtick)*SIMTICK, 0)/1000.0f);
        else if(spawncountdown(buf)) {}
        else if(splicetextuntil && totalmillis - splicetextuntil < 0) copystring(buf, "splice");
        if(!buf[0]) return;
        const float scale = 0.4f;
        int tw = 0, th = 0;
        text_bounds(buf, tw, th);
        pushhudmatrix();
        hudmatrix.scale(scale, scale, 1);
        flushhudmatrix();
        draw_text(buf, int(w/2.0f/scale - tw/2.0f), int(h/2.0f/scale + th/2), 255, 255, 255, 220);
        pophudmatrix();
    }

    // 0 replays the current run, 1 the one before it
    ICOMMAND(inputreplay, "i", (int *which),
    {
        inputrun &src = *which ? previous : recording;
        if(multiplayer(false) || src.empty()) { conoutf(CON_ERROR, "inputreplay: no recorded run"); return; }
        playinputrun(src);
    });

    static void writefromstate(const char *cmd, const char *file, const statestart &from)
    {
        bool replayed = replaying || (recordingoff && replaydata.attempts.inrange(replayattempt));
        const inputattempt *src = replayed ? &replaycur() : (recording.attempts.empty() ? NULL : &recording.last());
        if(!from.valid() || !src || src->ticks < from.tick)
        {
            conoutf(CON_ERROR, "%s: no saved state in the current attempt", cmd);
            return;
        }
        inputrun run;
        inputattempt &a = run.add();
        cutattempt(a, *src, from);
        bool finished = false;
        int runtime = runtimerresult(finished);
        replayinfo info;
        fillreplayinfo(run, finished, runtime, info);
        if(writereplay(file, run, info)) conoutf("%s: wrote %s (ticks %d-%d, %d events)", cmd, file, from.tick+1, a.ticks, a.events.length());
        else conoutf(CON_ERROR, "%s: could not write %s", cmd, file);
    }

    // Harness: writes the current attempt (of the recording, or of the replay that is playing or
    // has just ended) from the state saved with savestate, or from the start of player1's
    // current life.
    ICOMMAND(inputsavefromstate, "s", (char *file), writefromstate("inputsavefromstate", file, savedstart));
    ICOMMAND(inputsavelife, "s", (char *file), writefromstate("inputsavelife", file, lifestart));

    // Debug text dump of a run (0 = current, 1 = previous), readable by inputload.
    ICOMMAND(inputdump, "si", (char *file, int *which),
    {
        inputrun &run = *which ? previous : recording;
        if(run.empty()) { conoutf(CON_ERROR, "inputdump: no recorded run"); return; }
        stream *f = openutf8file(path(file, true), "w");
        if(!f) { conoutf(CON_ERROR, "inputdump: could not open %s", file); return; }
        int events = 0;
        int ticks = 0;
        loopvk(run.attempts)
        {
            const inputattempt &a = *run.attempts[k];
            f->printf("attempt %d %u %d %s %s %d %d %d\n", k+1, a.seed, a.mode, escapestring(a.map), escapestring(a.team), a.flagrun, a.ticks, a.race);
            loopv(a.settings) f->printf("s %s %d\n", escapestring(a.settings[i].name), a.settings[i].value);
            loopv(a.splices) f->printf("sp %d %d\n", a.splices[i].tick, a.splices[i].discarded);
            if(a.hascfgcrc) f->printf("cfgcrc %08x\n", a.cfgcrc);
            // the saved state as hex, 1024 bytes per line
            if(a.snapshot.length()) f->printf("snap %d\n", a.starttick);
            for(int pos = 0; pos < a.snapshot.length(); pos += 1024)
            {
                f->printf("sb ");
                for(int j = pos; j < min(pos + 1024, a.snapshot.length()); j++) f->printf("%02x", a.snapshot[j]);
                f->printf("\n");
            }
            loopv(a.events)
            {
                const inputevent &e = a.events[i];
                const inputcontrols &c = e.controls;
                f->printf("e %d %d %d %u %u %d %d %d %d %d %u %u %s\n", e.tick, e.type, e.arg, c.yaw, c.pitch,
                    c.move, c.strafe, c.vertical, c.jumping ? 1 : 0, c.attacking ? 1 : 0, e.cursorx, e.cursory,
                    escapestring(e.cmd ? e.cmd : ""));
            }
            loopv(a.hashes)
            {
                const tickhash &h = a.hashes[i];
                f->printf("h %d", h.tick);
                loopj(a.hashgroups) f->printf(" %u", h.groups[j]);
                f->printf("\n");
            }
            events += a.events.length();
            ticks += a.ticks;
        }
        delete f;
        conoutf("inputdump: wrote %d maps, %d events, %d ticks to %s", run.attempts.length(), events, ticks, file);
    });

    // explodelist keeps cubescript escapes (^", ^^) inside quoted elements
    static char *unescaped(const char *s)
    {
        int len = strlen(s);
        char *d = newstring(len);
        d[unescapestring(d, s, s + len)] = '\0';
        return d;
    }

    static void copyunescaped(char *dst, const char *s)
    {
        char *u = unescaped(s);
        copystring(dst, u, MAXSTRLEN);
        delete[] u;
    }

    // Loads an inputdump file into the previous-attempt slot and replays it.
    ICOMMAND(inputload, "s", (char *file),
    {
        if(multiplayer(false)) return;
        {
            inputrun run;
            replayinfo info;
            if(readreplay(file, &run, info) && !run.empty())
            {
                conoutf("inputload: %d maps, starting on %s", run.attempts.length(), run.attempts[0]->map);
                playinputrun(run);
                return;
            }
        }
        stream *f = openutf8file(path(file, true), "r");
        if(!f) { conoutf(CON_ERROR, "inputload: could not open %s", file); return; }
        inputrun &run = previous;
        run.clear();
        inputattempt *cur = NULL;
        char line[4096];
        while(f->getline(line, sizeof(line)))
        {
            vector<char *> w;
            explodelist(line, w);
            if(w.length() >= 6 && !strcmp(w[0], "attempt"))
            {
                inputattempt &a = *(cur = &run.add());
                a.seed = uint(strtoul(w[2], NULL, 10));
                a.mode = parseint(w[3]);
                copyunescaped(a.map, w[4]);
                copyunescaped(a.team, w[5]);
                a.flagrun = w.length() > 6 ? parseint(w[6]) : 0;
                a.ticks = w.length() > 7 ? parseint(w[7]) : 0;
                a.race = w.length() > 8 ? parseint(w[8]) : 0;
            }
            else if(cur && w.length() >= 2 && !strcmp(w[0], "snap")) cur->starttick = parseint(w[1]);
            else if(cur && w.length() >= 2 && !strcmp(w[0], "cfgcrc")) { cur->cfgcrc = uint(strtoul(w[1], NULL, 16)); cur->hascfgcrc = true; }
            else if(cur && w.length() >= 3 && !strcmp(w[0], "sp"))
            {
                inputsplice &sp = cur->splices.add();
                sp.tick = parseint(w[1]);
                sp.discarded = parseint(w[2]);
            }
            else if(cur && w.length() >= 2 && !strcmp(w[0], "sb"))
            {
                for(const char *c = w[1]; c[0] && c[1]; c += 2)
                {
                    char hex[3];
                    hex[0] = c[0]; hex[1] = c[1]; hex[2] = '\0';
                    cur->snapshot.add(uchar(strtoul(hex, NULL, 16)));
                }
            }
            else if(cur && w.length() >= 3 && !strcmp(w[0], "s"))
            {
                inputsetting &st = cur->settings.add();
                copyunescaped(st.name, w[1]);
                st.value = parseint(w[2]);
            }
            else if(cur && w.length() >= 13 && !strcmp(w[0], "e"))
            {
                inputevent &e = cur->events.add();
                e.tick = parseint(w[1]);
                e.type = parseint(w[2]);
                e.arg = parseint(w[3]);
                inputcontrols &c = e.controls;
                c.yaw = uint(strtoul(w[4], NULL, 10));
                c.pitch = uint(strtoul(w[5], NULL, 10));
                c.move = parseint(w[6]);
                c.strafe = parseint(w[7]);
                c.vertical = parseint(w[8]);
                c.jumping = parseint(w[9]) != 0;
                c.attacking = parseint(w[10]) != 0;
                e.cursorx = uint(strtoul(w[11], NULL, 10));
                e.cursory = uint(strtoul(w[12], NULL, 10));
                e.cmd = unescaped(w.length() > 13 ? w[13] : "");
            }
            else if(cur && w.length() >= 7 && !strcmp(w[0], "h"))
            {
                // the oldest dumps have one hash per tick, no tick number and 6 groups
                bool hastick = w.length() != 1 + DETHASH_LEGACYGROUPS;
                int first = hastick ? 2 : 1;
                int groups = min(w.length() - first, int(DETHASH_GROUPS));
                if(cur->hashes.empty()) cur->hashgroups = groups;
                tickhash &h = cur->hashes.add();
                h.tick = hastick ? parseint(w[1]) : cur->hashes.length();
                memset(h.groups, 0, sizeof(h.groups));
                loopi(groups) h.groups[i] = uint(strtoul(w[i+first], NULL, 10));
                if(!hastick) cur->ticks = max(cur->ticks, h.tick);
            }
            w.deletearrays();
        }
        delete f;
        if(run.empty()) { conoutf(CON_ERROR, "inputload: %s is not an input dump", file); run.clear(); return; }
        conoutf("inputload: %d maps, starting on %s", run.attempts.length(), run.attempts[0]->map);
        playinputrun(run);
    });

    ICOMMAND(inputevents, "", (), intret(replaying ? replaycur().events.length() : (recording.attempts.empty() ? 0 : recording.last().events.length())));
    ICOMMAND(inputmaps, "i", (int *which), intret((*which ? previous : recording).attempts.length()));
    ICOMMAND(inputdesync, "", (), intret(replaydesync));
    ICOMMAND(inputreplayed, "", (), intret(replayedticks));
    ICOMMAND(getclientmap, "", (), result(getclientmap()));
    static void getmapguis()
    {
        vector<char> names;
        float cx = 0, cy = 0;
        getmapguistate(names, cx, cy);
        result(names.getbuf());
    }
    COMMAND(getmapguis, "");
}
