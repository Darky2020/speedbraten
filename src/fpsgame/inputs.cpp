// Offline input stream. Live input that arrives between ticks is queued and applied at the
// start of the next tick, and everything the simulation reads from the player is recorded
// per tick, so the current attempt can be replayed exactly.

#include "game.h"
#include "sbcompat.h"
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
    static string replayfilename = ""; // file of the replay that is playing, "" if not from a file

    static void checkpendingreset();

    // movement, jump or attack input from the player (aim alone does not count)
    static void noteactivity()
    {
        if(player1->move || player1->strafe || player1->vertical || player1->jumping || player1->attacking) lastactivetick = curtick;
    }

    extern void saverunreplay(const inputrun &run, bool finished, int runtime, bool started);

    static const char * const simsettings[] = { "skill", "killsendsp", "slowmosp", "spawnwait", "attackspawn", "jumpspawn", "deadpush", "aiforcegun", "floatspeed", "racestart", "flagrunrestart" };
    enum { NUMSIMSETTINGS = sizeof(simsettings)/sizeof(simsettings[0]) };
    // settings added after replays started recording settings (the last ones of simsettings),
    // with the value that matches replays and saved states made before them
    static const struct { const char *name; int absent; } latesettings[] = { { "racestart", -1 }, { "flagrunrestart", 0 } };
    enum { NUMLATESETTINGS = sizeof(latesettings)/sizeof(latesettings[0]), NUMOLDSETTINGS = NUMSIMSETTINGS - NUMLATESETTINGS };
    static void readsettings(int *vals);

    // A saved state with the setting values in effect when it was taken, and the branch of the
    // recording it was taken on.
    struct statestart
    {
        vector<uchar> state;
        int tick, branch;
        int settings[NUMSIMSETTINGS];
        statestart *life; // start of the life the state was taken in, if known
        uint date;
        bool chained; // where a run of a life starts that goes on from the run before it

        statestart() : tick(-1), branch(-1), life(NULL), date(0), chained(false) {}
        bool valid() const { return tick >= 0; }
        void clear() { state.setsize(0); tick = branch = -1; life = NULL; date = 0; chained = false; }
    };

    // Segmented recordings form a tree: every live load starts a new branch at the loaded
    // state's tick, on the branch that state was taken on. A branch holds the input recorded
    // after its fork; the recording of the current attempt is always the current branch's
    // whole chain, so a state's history is its own chain however many loads came between.
    struct inputbranch
    {
        int parent, forktick, discarded, kind, wallms;
        vector<inputevent> events;
        vector<tickhash> hashes;
        vector<rawevent> raw;
        vector<aidmark> aids;

        inputbranch(int parent, int forktick, int discarded, int kind, int wallms) : parent(parent), forktick(forktick), discarded(discarded), kind(kind), wallms(wallms) {}
        ~inputbranch() { loopv(events) DELETEA(events[i].cmd); }
    };
    static vector<inputbranch *> branches;
    static int curbranch = -1;

    static void resetbranches()
    {
        branches.deletecontents();
        branches.add(new inputbranch(-1, 0, 0, inputsplice::SAVED, -1));
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
        loopv(br.raw) if(br.raw[i].tick <= limit) a.raw.add(br.raw[i]);
        loopv(br.aids) if(br.aids[i].tick <= limit) a.aids.add(br.aids[i]);
        if(br.parent >= 0)
        {
            inputsplice &sp = a.splices.add();
            sp.tick = br.forktick;
            sp.discarded = br.discarded;
            sp.kind = br.kind;
            sp.wallms = br.wallms;
        }
    }

    // Moves the current branch's input out of the recording into the tree, starts a branch at
    // tick of branch from, and makes the recording that branch's chain.
    // drops the timing of the segments after tick
    static void cuttiming(inputattempt &a, int tick)
    {
        int seg = tick / REPLAYSEGMENTTICKS;
        while(a.timing.length() && a.timing.last().segment > seg) a.timing.pop();
    }

    static void forkrecording(int from, int tick, int kind)
    {
        if(recording.attempts.empty() || curbranch < 0) return;
        inputattempt &rec = recording.last();
        inputbranch &cur = *branches[curbranch];
        loopv(rec.events) if(rec.events[i].tick > cur.forktick) { cur.events.add(rec.events[i]); rec.events[i].cmd = NULL; }
        loopv(rec.hashes) if(rec.hashes[i].tick > cur.forktick) cur.hashes.add(rec.hashes[i]);
        loopv(rec.raw) if(rec.raw[i].tick > cur.forktick) cur.raw.add(rec.raw[i]);
        loopv(rec.aids) if(rec.aids[i].tick > cur.forktick) cur.aids.add(rec.aids[i]);
        int discarded = rec.ticks - commontick(curbranch, rec.ticks, from, tick);
        curbranch = branches.length();
        branches.add(new inputbranch(from, tick, max(discarded, 0), kind, tellsrunms()));
        loopv(rec.events) DELETEA(rec.events[i].cmd);
        rec.events.setsize(0);
        rec.hashes.setsize(0);
        rec.raw.setsize(0);
        rec.aids.setsize(0);
        rec.splices.setsize(0);
        addchain(rec, curbranch, tick);
        rec.ticks = tick;
        cuttiming(rec, tick);
    }

    // The attempt from a saved state on: the state, then the events and hashes after its tick.
    static void cutattempt(inputattempt &a, const inputattempt &src, const statestart &from)
    {
        src.copyto(a);
        a.snapshot = from.state;
        a.starttick = from.tick;
        a.chained = from.chained;
        if(from.date) a.date = from.date;
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
        n = 0;
        loopv(a.timing) if(a.timing[i].segment < from.tick / REPLAYSEGMENTTICKS) n++; else break;
        a.timing.remove(0, n);
        n = 0;
        loopv(a.raw) if(a.raw[i].tick <= from.tick) n++; else break;
        a.raw.remove(0, n);
        // the last mark at or before the start still covers it
        n = 0;
        loopv(a.aids) if(a.aids[i].tick <= from.tick) n++; else break;
        if(n > 1) a.aids.remove(0, n-1);
    }

    // what cutattempt keeps of an attempt besides its events, hashes and timing
    static void lifeheader(inputattempt &a, const inputattempt &src, const statestart &from)
    {
        a.clear();
        a.settings = src.settings;
        loopv(a.settings) loopj(NUMSIMSETTINGS) if(!strcmp(a.settings[i].name, simsettings[j])) a.settings[i].value = from.settings[j];
        a.snapshot = from.state;
        a.starttick = from.tick;
        a.files = src.files;
        a.seed = src.seed;
        a.mapcrc = src.mapcrc;
        a.date = from.date ? from.date : src.date;
        a.cfgcrc = src.cfgcrc;
        a.hascfgcrc = src.hascfgcrc;
        copystring(a.map, src.map);
        copystring(a.team, src.team);
        copystring(a.player, src.player);
        a.mode = src.mode;
        a.flagrun = src.flagrun;
        a.race = src.race;
        a.ticks = src.ticks;
        a.hashgroups = src.hashgroups;
        a.lifeseeds = src.lifeseeds;
        a.chained = from.chained;
    }

    extern int fixedgameseed;

    // the current attempt's seed came from the witness, so its runs are streamed to it
    static bool attemptwitnessed = false;
    static statestart *lifestart;

    // streams the current life; the attempt's map files and CRCs are only known from tick 1 on
    static void witnesslife(int continues = -1)
    {
        if(!attemptwitnessed || replaying || recordingoff || !lifestart || recording.attempts.length() != 1) return;
        inputattempt header;
        lifeheader(header, recording.last(), *lifestart);
        witnessrunstart(header, recording.last(), lifestart->tick, true, continues);
    }

    // Flagruns and races are judged per life: the replay of a finished run starts at the
    // spawn of the life it was run in, and an unfinished life whose run started is saved
    // on its own when it ends.
    MOD(VARP, replaylifeonly, 0, 1, 1);
    // lifestart: start of the current life, one of lives, which also keeps the life starts that
    // saved states still refer to
    static statestart savedstart;
    static vector<statestart *> lives;
    static const statestart nolife;
    static bool lifepending = false, lifesaved = false, chainpending = false;
    static int chainfrom = -1; // the witnessed run the pending chained start goes on from
    static int lifesavedbranch = -1, lifesavedtick = 0; // where the saved replay of this life ends

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
        if(spectatehold()) return true;
        if(!holduntil) return false;
        if(totalmillis - holduntil < 0) return true;
        holduntil = 0;
        return false;
    }

    // A live load keeps the keys the player holds now: the state's movement and attack belong
    // to the moment it was saved, and the keys pressed then are not held any more.
    static void hubviewfrom(const statestart &st);

    static bool loadlivestate(statestart &st, int kind)
    {
        fpsent &d = *player1;
        bool keys[6] = { d.k_left, d.k_right, d.k_forward, d.k_backward, d.k_up, d.k_down };
        schar move = d.move, strafe = d.strafe, vertical = d.vertical;
        bool jumping = d.jumping, attacking = d.attacking;
        if(!replaying && !recordingoff) witnessstop("a state was loaded");
        hubviewstop();
        chainpending = false;
        if(!restorestate(st.state)) return false;
        if(!replaying && !recordingoff) { forkrecording(st.branch, st.tick, kind); tellsload(); hubviewfrom(st); }
        // back to the life the state was taken in, which may have ended since
        if(st.life && onchain(st.life->branch, st.life->tick, st.branch, st.tick)) lifestart = st.life;
        else if(lifestart && !onchain(lifestart->branch, lifestart->tick, st.branch, st.tick)) lifestart = NULL;
        if(lifesaved && !onchain(lifesavedbranch, lifesavedtick, st.branch, st.tick)) lifesaved = false;
        while(autostates.length() && autostates.last()->tick > st.tick) delete autostates.pop();
        d.k_left = keys[0]; d.k_right = keys[1]; d.k_forward = keys[2]; d.k_backward = keys[3]; d.k_up = keys[4]; d.k_down = keys[5];
        d.move = move; d.strafe = strafe; d.vertical = vertical;
        d.jumping = jumping; d.attacking = attacking;
        return true;
    }

    static void loadautostate();

    bool loadsavedstate() { return savedstart.valid() && loadlivestate(savedstart, inputsplice::SAVED); }

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
            savedstart.life = lifestart;
            readsettings(savedstart.settings);
            if(!replaying && !recordingoff) tellssave();
        }
        if(autoloadpending) { autoloadpending = false; loadautostate(); }
    }

    static bool havebots()
    {
        loopv(players) if(players[i]->ai) return true;
        return false;
    }

    static bool liferefered(const statestart *l)
    {
        if(l == lifestart || l == savedstart.life) return true;
        loopv(autostates) if(autostates[i]->life == l) return true;
        return false;
    }

    static void takelifestate()
    {
        lifepending = false;
        lifestart = NULL;
        loopvrev(lives) if(!liferefered(lives[i])) delete lives.remove(i);
        statestart *l = new statestart;
        if(!replaylifeonly || havebots() || !takesnapshot(l->state)) { delete l; return; }
        l->tick = curtick;
        l->branch = curbranch;
        l->date = uint(time(NULL));
        readsettings(l->settings);
        lives.add(l);
        lifestart = l;
        tellslifestart();
        if(curtick >= 1) witnesslife();
    }

    // After a witnessed run ends in a life that goes on, the life's next run starts at the
    // following tick: a start taken like a life's, without a spawn or a reseed, which the
    // witness checks by playing on from where it re-simulated the run before.
    static void takechainstate()
    {
        chainpending = false;
        statestart *l = new statestart;
        if(!lifestart || !takesnapshot(l->state)) { delete l; return; }
        l->tick = curtick;
        l->branch = curbranch;
        l->date = uint(time(NULL));
        l->chained = true;
        readsettings(l->settings);
        lives.add(l);
        lifestart = l;
        lifesaved = false;
        witnesslife(chainfrom);
    }

    // after a chained start, only a run begun since counts
    static bool liferunstarted()
    {
        if(lifestart && lifestart->chained) return runtimerstarttick() > lifestart->tick;
        return runtimerliferan();
    }

    static bool saveliferun(bool finished, int runtime)
    {
        if(!replaylifeonly || !lifestart || lifesaved || recording.attempts.length() != 1) return false;
        if(!finished && !liferunstarted()) return true;
        inputrun run;
        cutattempt(run.add(), recording.last(), *lifestart);
        tellssavinglife(true);
        saverunreplay(run, finished, runtime, true);
        tellssavinglife(false);
        lifesaved = true;
        lifesavedbranch = curbranch;
        lifesavedtick = curtick;
        return true;
    }

    static void seedlife();

    void inputlifespawned()
    {
        if(runtimerliferan()) recordingran = true;
        if(!replaying && !recordingoff) saveliferun(false, 0);
        lifestart = NULL;
        lifesaved = chainpending = false;
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
        st->life = lifestart;
        readsettings(st->settings);
        autostates.add(st);
        while(autostates.length() > autostatecount) delete autostates.remove(0);
    }

    // a spectator view from a saved state of the current attempt
    static void hubviewfrom(const statestart &st)
    {
        if(replaying || recordingoff || recording.attempts.empty()) return;
        inputattempt header;
        lifeheader(header, recording.last(), st);
        hubviewbegin(header, recording.last(), st.tick);
    }

    // A new spectator view from a snapshot of now: every HUBKEYFRAMETICKS, so a late spectator
    // catches up through at most that, and when streaming becomes wanted mid-attempt.
    enum { HUBKEYFRAMETICKS = 12000 };
    static void hubkeyframe()
    {
        if(replaying || recordingoff || curtick <= 1 || recording.attempts.empty()) return;
        if(hubviewing() ? curtick - hubviewstart() < HUBKEYFRAMETICKS : curtick % 200 || !hubstreaming()) return;
        statestart st;
        if(!takesnapshot(st.state)) return;
        st.tick = curtick;
        st.branch = curbranch;
        st.date = uint(time(NULL));
        readsettings(st.settings);
        hubviewfrom(st);
    }

    // loads the newest automatic state that lies at least a quarter interval back
    static void loadautostate()
    {
        if(replaying || replaypending) { conoutf(CON_ERROR, "savestates do not work during a replay"); return; }
        int margin = max(autostateinterval / SIMTICK / 4, 1);
        for(int i = autostates.length()-1; i >= 0; i--) if(autostates[i]->tick <= curtick - margin)
        {
            int back = curtick - autostates[i]->tick;
            if(loadlivestate(*autostates[i], inputsplice::AUTO))
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
        else if(lifesaved && (chainfrom = witnesstakefinished()) >= 0) chainpending = true;
        runsaved = true;
    }

    static void restoresettings();

    void inputdisconnect()
    {
        saveunfinishedrun();
        witnessstop("disconnected");
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

    static void feedevents();
    static void feedcontrols();

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
        e.source = inputsource;
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
        return insimtick && (identflags&IDF_OVERRIDDEN) && m_check(mode, M_DMSP | M_CLASSICSP) && !runtimerfinishedbeforemap();
    }

    static inputattempt &replaycur() { return *replaydata.attempts[replayattempt]; }

    bool replaylifecheck = false;
    string replaylifecheckresult = "skipped";
    enum { LIFECHECKWAIT = 400 };

    static uint lifeseed(uint seed, int life)
    {
        uint h = seed ^ (uint(life) * 0x9E3779B1U);
        h ^= h >> 16; h *= 0x85EBCA6BU; h ^= h >> 13; h *= 0xC2B2AE35U; h ^= h >> 16;
        return h;
    }

    // At the start of the tick after a spawn in a flagrun or race, where the life's saved state
    // is taken, the RNG is reseeded: a life then starts from the same RNG state whatever came
    // before it in the session, and can be checked on its own.
    static void seedlife()
    {
        const inputattempt *a = replaying ? &replaycur() : !recordingoff && recording.attempts.length() ? &recording.last() : NULL;
        if(!a || !a->lifeseeds || (!a->flagrun && !a->race)) return;
        seedMT(lifeseed(a->seed, player1->lifesequence));
    }

    // Settings the simulation reads that are not part of the player's input. A recording
    // stores them at the start of each attempt and records every later change; a replay
    // plays with the recorded values and then gives the player's own values back.
    static int cursettings[NUMSIMSETTINGS], usersettings[NUMSIMSETTINGS];
    // the settings after the last frame that ran a tick, and the ticks recorded this frame
    static int framesettings[NUMSIMSETTINGS], frameticks = 0;
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

    // Records the settings changed since the last tick, other than through recorded input. The
    // player can only change one between frames; a change at the first tick comes from the map
    // load, and one made during a tick (another tick already ran this frame, or the value was
    // there when the last frame's ticks ended) comes from the map's script.
    static void recordsettingchanges(inputattempt &a)
    {
        loopi(NUMSIMSETTINGS)
        {
            int val = getsetting(i);
            if(val == cursettings[i]) continue;
            bool map = curtick <= 1 || frameticks > 0 || val == framesettings[i];
            cursettings[i] = val;
            inputevent &e = a.events.add();
            e.tick = curtick;
            e.type = INPUT_SETTING;
            e.source = map ? INPUTSRC_MAP : INPUTSRC_RECORDER;
            e.arg = val;
            memset(&e.controls, 0, sizeof(e.controls));
            e.cursorx = e.cursory = 0;
            e.cmd = newstring(simsettings[i]);
        }
    }

    bool queuemenuinput(const char *cmd, bool scripted)
    {
        if(!simticking || insimtick || !connected) return false;
        if(scripted) queueinput(INPUT_MAPGUI, 0, cmd);
        else deferinput(INPUT_MAPGUI, 0, cmd);
        return true;
    }

    static void applyevent(const inputevent &e)
    {
        switch(e.type)
        {
            case INPUT_CONTROLS: lastcontrols = e.controls; break;
            case INPUT_JUMP: if(canjump()) player1->jumping = true; break;
            case INPUT_ATTACK: doattack(e.arg != 0); break;
            // arg 1: a command outside the replay command list, read from a replay file
            case INPUT_COMMAND:
                if(!e.cmd) break;
                if(e.arg) conoutf(CON_ERROR, "replay command refused: %s", e.cmd);
                else execute(e.cmd);
                break;
            case INPUT_MAPGUI: if(e.cmd && mapguiontop()) execute(e.cmd); break;
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

    static const mapdatafile *finddatafile(const vector<mapdatafile> &files, const char *path)
    {
        loopv(files) if(!strcmp(files[i].path, path)) return &files[i];
        return NULL;
    }

    static void checkdatafiles(const vector<mapdatafile> &recorded)
    {
        const vector<mapdatafile> &loaded = getmapdatafiles();
        loopv(recorded)
        {
            const mapdatafile *f = finddatafile(loaded, recorded[i].path);
            if(!f) conoutf(CON_ERROR, "replay was recorded with %s, which this game did not load", recorded[i].path);
            else if(f->size != recorded[i].size || memcmp(f->hash, recorded[i].hash, sizeof(f->hash)))
                conoutf(CON_ERROR, "replay was recorded with a different %s: it will likely not play back correctly", recorded[i].path);
        }
        loopv(loaded) if(!finddatafile(recorded, loaded[i].path))
            conoutf(CON_ERROR, "replay was recorded without %s: it will likely not play back correctly", loaded[i].path);
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
        if(want.combined)
        {
            if(replaydesync || combinedhash(h.groups, a.hashgroups) == want.groups[0]) return;
            replaydesync = curtick;
            conoutf(CON_ERROR, "replay desync at tick %d (%.3f s)", curtick, (curtick-1)*SIMTICK/1000.0f);
            return;
        }
        vector<char> diff;
        bool named = replaydesyncgroups != 0;
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
        else if(!named) conoutf(CON_ERROR, "replay desync, by tick %d (%.3f s): %s", curtick, (curtick-1)*SIMTICK/1000.0f, diff.getbuf());
        else logoutf("replay desync spreads at tick %d (%.3f s): %s", curtick, (curtick-1)*SIMTICK/1000.0f, diff.getbuf());
    }

    // On load during a recording, the attempt is cut back to the restored tick, so it still
    // replays from map start: the restored state is the state it had at that tick.
    // saved states: section SETX holds the first late setting, SETN a count and the rest, so
    // states saved with fewer of them still restore
    void snaplatesettings(snapio &s) { s.i(cursettings[NUMOLDSETTINGS]); }
    void resetlatesettings(snapio &s) { loopi(NUMLATESETTINGS) cursettings[NUMOLDSETTINGS + i] = latesettings[i].absent; }
    void snapmorelatesettings(snapio &s)
    {
        int n = s.count(NUMLATESETTINGS - 1, 1000);
        loopi(n)
        {
            int k = NUMOLDSETTINGS + 1 + i, v = k < NUMSIMSETTINGS ? cursettings[k] : 0;
            s.i(v);
            if(s.loading && k < NUMSIMSETTINGS) cursettings[k] = v;
        }
        if(s.loading) for(int i = n + 1; i < NUMLATESETTINGS; i++) cursettings[NUMOLDSETTINGS + i] = latesettings[i].absent;
    }
    void resetmorelatesettings(snapio &s) { for(int i = 1; i < NUMLATESETTINGS; i++) cursettings[NUMOLDSETTINGS + i] = latesettings[i].absent; }

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
            inputattempt &a = replaycur();
            bool lifecheck = replaylifecheck && (a.flagrun || a.race);
            // the fresh state to check a life against is taken where a life's state is taken:
            // at the start of the tick after the spawn, here the map start's
            static int lifecheckwait = 0;
            if(lifecheck && !lifepending && lifecheckwait++ < LIFECHECKWAIT) return;
            if(lifecheck && !lifepending) copystring(replaylifecheckresult, "no spawn at the map start");
            lifecheckwait = 0;
            replaystartstate = false;
            if(lifecheck && lifepending) lifestartfresh();
            if(!restorereplaystart(a.snapshot))
            {
                conoutf(CON_ERROR, "the replay's saved state does not fit this map or build: replay stopped");
                replaying = false;
                recordingoff = true;
                restoresettings();
            }
            else
            {
                if(lifecheck && !strcmp(replaylifecheckresult, "skipped"))
                    lifestartcompare(a.lifeseeds, lifeseed(a.seed, player1->lifesequence), replaylifecheckresult, sizeof(replaylifecheckresult));
                replaypos = 0;
                while(replaypos < a.events.length() && a.events[replaypos].tick <= curtick) replaypos++;
                replayhashpos = 0;
            }
        }
        replaystartstate = false;
        if(lifepending) { seedlife(); takelifestate(); }
        else if(chainpending) takechainstate();
        runstateops();
        snapcycletick();
        takeautostate();
        hubkeyframe();
        if(replaying && replaychainhook && replayattempt == replaydata.attempts.length()-1 && curtick+1 > replaycur().ticks) replaychainhook();
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
            if(curtick == 1 && a.files.length()) checkdatafiles(a.files);
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
            rec.files = getmapdatafiles();
            if(attemptwitnessed && !(replaylifeonly && (rec.flagrun || rec.race)))
            {
                if(recording.attempts.length() == 1) witnessrunstart(rec, rec, -1, false);
                else witnessnextattempt(rec);
            }
            else witnesslife();
            if(recording.attempts.length() == 1 || !hubviewing()) hubviewbegin(rec, rec, -1);
            else hubviewnextattempt(rec);
        }
        recordsettingchanges(rec);
        feedevents();
        while(pending.length())
        {
            inputevent e = pending.remove(0);
            e.tick = curtick;
            applyevent(e);
            if(curtick != e.tick) { DELETEA(e.cmd); return; }
            rec.events.add(e);
        }
        feedcontrols();
        readsettings(cursettings);
        inputcontrols c;
        getcontrols(c);
        if(!havecontrols || c != lastcontrols)
        {
            inputevent &e = rec.events.add();
            e.tick = curtick;
            e.type = INPUT_CONTROLS;
            e.source = INPUTSRC_RECORDER;
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
            e.source = INPUTSRC_RECORDER;
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
            e.source = INPUTSRC_RECORDER;
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
        frameticks++;
    }

    // harnessfeed: plays a replay's input into the next live attempt as if it were typed, so
    // the live recorder records it (the determinism harness's stand-in for a player)
    static inputrun feedrun;
    static int feedpos = 0, feeduntil = 0;
    static bool feeding = false, feedseed = false;
    static inputcontrols feedlast;

    static void feedevents()
    {
        if(!feeding) return;
        const inputattempt &a = *feedrun.attempts[0];
        if(strcmp(getclientmap(), a.map)) return;
        if(curtick == 1) feedpos = 0;
        while(feedpos < a.events.length() && a.events[feedpos].tick < curtick) feedpos++;
        for(int i = feedpos; i < a.events.length() && a.events[i].tick == curtick; i++)
        {
            const inputevent &e = a.events[i];
            if(e.type == INPUT_CONTROLS) feedlast = e.controls;
            else if(e.type == INPUT_JUMP || e.type == INPUT_ATTACK || e.type == INPUT_COMMAND)
            {
                inputevent &p = pending.add(e);
                if(p.cmd) p.cmd = newstring(p.cmd);
            }
        }
        if((feedpos >= a.events.length() && curtick > a.ticks) || (feeduntil > 0 && curtick >= feeduntil)) { feeding = false; conoutf("harnessfeed: done"); }
    }

    static void feedcontrols() { if(feeding) setcontrols(feedlast); }

#ifndef SPEEDBRATEN_RELEASE
    // until: the last tick to feed, 0 for the whole replay; ownseed: start with the replay's seed
    ICOMMAND(harnessfeed, "sii", (char *file, int *until, int *ownseed),
    {
        tellsharness();
        replayinfo info;
        feeding = false;
        if(!readreplay(file, &feedrun, info) || feedrun.empty()) { conoutf(CON_ERROR, "harnessfeed: could not read %s", file); return; }
        getcontrols(feedlast);
        feedpos = 0;
        feeduntil = *until;
        feedseed = *ownseed != 0;
        feeding = true;
        forcesettings(*feedrun.attempts[0]);
        applysettings();
    });
#endif

    static void seedattempt(const inputattempt &a)
    {
        gameseed = a.seed;
        seedMT(gameseed);
        logoutf("game seed: %u (replay of %s, %d events)", gameseed, a.map, a.events.length());
    }

    void inputmapstart(const char *name, int mode)
    {
        bool chained = mapchained(mode) && !recordingoff, unstarted = lifestart && !lifesaved && !liferunstarted();
        if(!chained || replaypending) { saveunfinishedrun(); hubviewstop(); }
        lifestart = NULL;
        lives.deletecontents();
        lifepending = lifesaved = chainpending = false;
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
            if(chained && replayattempt+1 >= replaydata.attempts.length() && replaywaitattempt) replaywaitattempt();
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
        if(chained) {}
        else if(unstarted) witnessdrop("the map changed");
        else witnessstop("the map changed");
        seedgame();
        uint seed;
        // a campaign run that starts after the campaign's first map is practice: no witness
        const char *firstmap = chained && recording.attempts.length() ? recording.attempts[0]->map : name;
        attemptwitnessed = !fixedgameseed && campaignmap(firstmap, NULL) <= 0 && witnessseed(seed);
        if(attemptwitnessed) { gameseed = seed; seedMT(gameseed); logoutf("game seed: %u (from the witness)", gameseed); }
        else if(feeding && feedseed && !strcmp(name, feedrun.attempts[0]->map)) { gameseed = feedrun.attempts[0]->seed; seedMT(gameseed); logoutf("game seed: %u (harnessfeed)", gameseed); }
        if(!chained && !recording.empty()) recording.moveto(previous);
        if(!chained) { recording.clear(); runsaved = false; recordingran = false; tellsrunstart(); }
        inputattempt &a = recording.add();
        a.seed = gameseed;
        a.date = uint(time(NULL));
        copystring(a.player, player1->name);
        a.lifeseeds = true;
        copystring(a.map, name);
        a.mode = mode;
        a.flagrun = server::flagrunactive() ? 1 : 0;
        a.race = server::racerulesactive() ? 1 : 0;
        recordsettings(a);
        readsettings(framesettings);
        frameticks = 0;
    }

    const char *internbind(const char *s, int len)
    {
        static vector<char *> pool;
        loopv(pool) if(int(strlen(pool[i])) == len && !strncmp(pool[i], s, len)) return pool[i];
        return pool.add(newstring(s, len));
    }

    void rawinput(const rawevent &e)
    {
        if(!simticking || replaying || recordingoff || recording.attempts.empty() || !connected) return;
        rawevent &r = recording.last().raw.add(e);
        r.tick = curtick + 1;
        r.us = tellsrunus();
        if(r.bind) r.bind = internbind(r.bind, strlen(r.bind));
    }

    // the aids on during a frame count for the tick the frame leads up to
    static void markaids(inputattempt &a, int tick, int mask)
    {
        if(a.aids.length() && a.aids.last().tick == tick) { a.aids.last().mask |= mask; return; }
        if(a.aids.length() ? a.aids.last().mask == mask : !mask) return;
        aidmark &m = a.aids.add();
        m.tick = tick;
        m.mask = mask;
    }

    int inputrunaids(const inputrun &r)
    {
        int mask = 0;
        loopv(r.attempts) loopvj(r.attempts[i]->aids) mask |= r.attempts[i]->aids[j].mask;
        return mask;
    }

    void frametimed(int us, int ticks, bool drawn)
    {
        witnessupdate();
        hubupdate();
        int aids = tellsframe(us);
        if(frameticks) readsettings(framesettings);
        frameticks = 0;
        if(replaying || recordingoff || recording.attempts.empty() || !connected) return;
        inputattempt &a = recording.last();
        markaids(a, max(curtick, 0) + 1, aids);
        if(tellsassistframe(aids)) witnessstop("an assist was used");
        int seg = max(curtick, 0) / REPLAYSEGMENTTICKS;
        cuttiming(a, curtick);
        if(a.timing.empty() || a.timing.last().segment != seg) a.timing.add(segmenttiming(seg));
        segmenttiming &t = a.timing.last();
        static const int limits[segmenttiming::BUCKETS-1] = { 2000, 4000, 8000, 16000, 33000, 66000, 250000 };
        int bucket = 0;
        while(bucket < segmenttiming::BUCKETS-1 && us >= limits[bucket]) bucket++;
        t.hist[bucket]++;
        t.wallus += us;
        // the hold after a live load isn't a pause: a life with a load in it is segmented anyway,
        // and a life that starts after one would otherwise take over its second's hold
        if(ispaused()) t.pausedus += us;
        if(!ticks) t.frames0++;
        else if(ticks >= 2) t.frames2++;
        if(drawn) t.drawn++;
        int speed = scaletime(1);
        if(!t.frames++) t.speedmin = t.speedmax = speed;
        else { t.speedmin = min(t.speedmin, speed); t.speedmax = max(t.speedmax, speed); }
        t.fixedframe = max(t.fixedframe, fixedframe);
        witnessframe(curtick);
        hubviewframe(curtick);
    }

    void playinputrun(const inputrun &src, const char *file)
    {
        copystring(replayfilename, file ? file : "");
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

    // A replay whose file is still being written (sauer_resim -L). extend copies in what is
    // known now; until the file is complete every attempt's length is unbounded. appendonly
    // leaves the attempts already there alone, for use inside a tick.
    bool (*replaywaitattempt)() = NULL;

    void inputreplayextend(const inputrun &src, bool complete, bool appendonly)
    {
        loopv(src.attempts)
        {
            bool known = i < replaydata.attempts.length();
            inputattempt &a = known ? *replaydata.attempts[i] : replaydata.add();
            if(!known || !appendonly) src.attempts[i]->copyto(a);
            a.ticks = complete ? src.attempts[i]->ticks : INT_MAX;
        }
    }

    // the replay is about to play a tick of its last known attempt at or after known
    bool inputreplayneeds(int known)
    {
        return replaying && replayattempt == replaydata.attempts.length()-1 && curtick+1 >= known;
    }

    int inputreplayattempts() { return replaydata.attempts.length(); }

    // Called where a replay of a whole file would end, after its last tick; it may hand
    // over the next run of the same life with inputreplaycontinue.
    void (*replaychainhook)() = NULL;

    // Goes on with a run that continues the one just replayed, from the state that replay
    // left: its events and hashes after the current tick, without its saved start state.
    bool inputreplaycontinue(const inputrun &src)
    {
        if(!replaying || src.attempts.length() != 1) return false;
        const inputattempt &n = *src.attempts[0];
        if(strcmp(n.map, replaycur().map) || n.mode != replaycur().mode || n.starttick > curtick) return false;
        src.copyto(replaydata);
        replayattempt = 0;
        const inputattempt &a = replaycur();
        replaypos = 0;
        while(replaypos < a.events.length() && a.events[replaypos].tick <= curtick) replaypos++;
        replayhashpos = 0;
        replaydesync = replaydesyncgroups = 0;
        replayedticks = 0;
        memset(lastreplayhash, 0, sizeof(lastreplayhash));
        copystring(replaylifecheckresult, "chained");
        return true;
    }
    // Plays a run that starts from a saved state on the map being replayed in place of the
    // replay: its state is restored at the next tick, without loading the map.
    bool inputreplayswitch(const inputrun &src)
    {
        if(!replaying || src.attempts.empty()) return false;
        const inputattempt &n = *src.attempts[0];
        if(n.snapshot.empty() || strcmp(n.map, replaycur().map) || n.mode != replaycur().mode) return false;
        src.copyto(replaydata);
        replayattempt = 0;
        replaypos = replayhashpos = 0;
        replaydesync = replaydesyncgroups = 0;
        replayedticks = 0;
        memset(lastreplayhash, 0, sizeof(lastreplayhash));
        seedattempt(replaycur());
        forcesettings(replaycur());
        replaystartstate = true;
        return true;
    }

    bool inputrecordinglive() { return !replaying && !replaypending && !recordingoff; }
    // the replay file being watched, "" when none or not played from a file
    const char *inputwatchedfile() { return replaying || replaypending ? replayfilename : ""; }
    int inputattemptindex() { return replaying ? replayattempt : max(recording.attempts.length()-1, 0); }

    // the first attempt of the run being played or replayed (its map and mode name the run)
    const inputattempt *inputrunfirst()
    {
        const inputrun &run = replaying ? replaydata : recording;
        return run.attempts.length() ? run.attempts[0] : NULL;
    }

    // the map and mode a finished single player run starts over on: its first map
    const char *sprestartmap(int &mode)
    {
        if(spectating()) return NULL;
        const inputattempt *a = inputrunfirst();
        if(!a || !a->map[0]) return NULL;
        mode = a->mode;
        return a->map;
    }

    // state loads in the run being played: for a flagrun or race life saved on its own, only
    // those since the life started, as in its replay
    int inputsplicecount()
    {
        const inputrun &run = replaying ? replaydata : recording;
        int n = 0;
        if(!replaying && replaylifeonly && lifestart && run.attempts.length() == 1)
        {
            loopv(run.attempts[0]->splices) if(run.attempts[0]->splices[i].tick > lifestart->tick) n++;
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
        tellsreset();
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
    ICOMMAND(inputsavelife, "s", (char *file), writefromstate("inputsavelife", file, lifestart ? *lifestart : nolife));

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
                e.source = INPUTSRC_OTHER;
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
