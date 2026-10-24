// Recorded input of offline games (see inputs.cpp) and the replay file format.

#ifndef INPUTS_H
#define INPUTS_H

namespace game
{
    enum { REPLAYSEGMENTTICKS = 200 };

    struct inputcontrols
    {
        uint yaw, pitch;
        schar move, strafe, vertical;
        bool jumping, attacking;

        bool operator==(const inputcontrols &o) const
        {
            return yaw == o.yaw && pitch == o.pitch && move == o.move && strafe == o.strafe &&
                   vertical == o.vertical && jumping == o.jumping && attacking == o.attacking;
        }
        bool operator!=(const inputcontrols &o) const { return !(*this == o); }
    };

    struct inputevent
    {
        int tick, type, arg;
        int source; // INPUTSRC_*
        inputcontrols controls;
        uint cursorx, cursory;
        char *cmd;
    };

    struct tickhash
    {
        int tick;
        uint groups[DETHASH_GROUPS];
        bool combined; // groups[0] holds combinedhash() of the groups, the rest is unknown

        tickhash() : tick(0), combined(false) {}
    };

    // one value standing for all groups of a state hash
    static inline uint combinedhash(const uint *groups, int n)
    {
        uchar b[4*DETHASH_GROUPS];
        loopi(n) loopj(4) b[4*i + j] = uchar(groups[i] >> (8*j));
        return uint(crc32(crc32(0, Z_NULL, 0), b, 4*n));
    }

    // a saved state was loaded at this tick of the attempt, throwing away the ticks played after it
    struct inputsplice
    {
        enum { SAVED = 0, AUTO };
        int tick, discarded;
        int kind;
        int wallms; // wall time since the run began, -1 unknown

        inputsplice() : tick(0), discarded(0), kind(SAVED), wallms(-1) {}
    };

    // wall clock of the frames that ended in one segment of an attempt
    // The aids on from tick on, until the next mark (bits of the tells feature list).
    struct aidmark { int tick, mask; };

    struct segmenttiming
    {
        enum { BUCKETS = 8 };
        int segment;
        long long wallus, pausedus;
        int frames, frames0, frames2; // main loop passes: all, with no tick, with 2 or more ticks
        int drawn;                    // passes that drew the screen, -1 unknown
        int hist[BUCKETS];            // frame times: <2, <4, <8, <16, <33, <66, <250, >=250 ms
        int speedmin, speedmax;       // gamespeed
        int fixedframe;               // highest fixedframe

        segmenttiming(int segment = 0) : segment(segment), wallus(0), pausedus(0), frames(0), frames0(0), frames2(0), drawn(0), speedmin(0), speedmax(0), fixedframe(0)
        {
            memset(hist, 0, sizeof(hist));
        }
    };

    // a chunk of run-level metadata in replay-file form
    struct runchunk
    {
        char type[5];
        bool late; // stored after the attempts
        vector<uchar> data;

        runchunk() : late(false) { type[0] = '\0'; }
    };

    // player1 on the run timer's clock, for drawing the run as a ghost
    struct ghostsample
    {
        enum { MOVESHIFT = 0, STRAFESHIFT = 2, INWATER = 1<<4, INAIR = 1<<5, ATTACK = 1<<6, GUNSHIFT = 8 };
        int tick;   // run timer tick
        vec o;
        float yaw, pitch;
        int flags;  // move+1, strafe+1, INWATER, INAIR, ATTACK, gun
    };

    struct inputsetting
    {
        string name;
        int value;
    };

    // One attempt: everything needed to replay it from its map start, plus the state hash
    // at the start of every tick to find where a replay drifts.
    struct inputattempt
    {
        vector<inputevent> events;
        vector<tickhash> hashes;
        vector<inputsetting> settings; // values at the start of the attempt
        vector<uchar> snapshot;          // saved state the attempt starts from instead of map start
        int starttick;                   // tick of the snapshot, 0 without one
        vector<inputsplice> splices;
        vector<ghostsample> trace; // finished or saved runs only
        vector<mapdatafile> files; // what the map load read
        vector<segmenttiming> timing; // by segment
        vector<rawevent> raw;         // binds point into internbind's pool
        vector<aidmark> aids;         // live recordings only
        uint seed, mapcrc;
        uint date;      // unix time the attempt (or its life) started, 0 if unknown
        uint cfgcrc;    // crc of the map cfg, 0 without one
        bool hascfgcrc; // false in the oldest replays, which have no cfg crc
        string map, team, player;
        int mode, flagrun, race, ticks;
        int hashgroups; // groups stored per hash: DETHASH_LEGACYGROUPS in the oldest replays
        bool lifeseeds; // in flagruns and races, every spawn reseeds the RNG (lifeseed())
        bool chained;   // continues the previous run of the same life from where it ended, without a spawn

        inputattempt() : starttick(0), seed(0), mapcrc(0), date(0), cfgcrc(0), hascfgcrc(false), mode(0), flagrun(0), race(0), ticks(0), hashgroups(DETHASH_GROUPS), lifeseeds(false), chained(false) { map[0] = team[0] = player[0] = '\0'; }
        ~inputattempt() { clear(); }

        void clear()
        {
            loopv(events) DELETEA(events[i].cmd);
            events.setsize(0);
            hashes.setsize(0);
            settings.setsize(0);
            snapshot.setsize(0);
            starttick = 0;
            splices.setsize(0);
            trace.setsize(0);
            files.setsize(0);
            timing.setsize(0);
            raw.setsize(0);
            aids.setsize(0);
        }

        bool empty() const { return events.empty(); }

        void copyto(inputattempt &dst) const
        {
            dst.clear();
            loopv(events)
            {
                inputevent &e = dst.events.add(events[i]);
                if(e.cmd) e.cmd = newstring(e.cmd);
            }
            dst.hashes = hashes;
            dst.settings = settings;
            dst.snapshot = snapshot;
            dst.starttick = starttick;
            dst.splices = splices;
            dst.trace = trace;
            dst.files = files;
            dst.timing = timing;
            dst.raw = raw;
            dst.aids = aids;
            dst.seed = seed;
            dst.mapcrc = mapcrc;
            dst.date = date;
            copystring(dst.player, player);
            dst.cfgcrc = cfgcrc;
            dst.hascfgcrc = hascfgcrc;
            copystring(dst.map, map);
            copystring(dst.team, team);
            dst.mode = mode;
            dst.flagrun = flagrun;
            dst.race = race;
            dst.hashgroups = hashgroups;
            dst.lifeseeds = lifeseeds;
            dst.chained = chained;
            dst.ticks = ticks;
        }
    };

    // A run is the chain of attempts of a multi-map singleplayer run (usually just one map).
    struct inputrun
    {
        vector<inputattempt *> attempts;
        vector<runchunk *> tells; // read from a file; a live run's are made when it is saved

        ~inputrun() { clear(); }
        void clear() { attempts.deletecontents(); tells.deletecontents(); }
        bool empty() const { return attempts.empty() || (attempts.length() == 1 && attempts[0]->empty()); }
        inputattempt &last() { return *attempts.last(); }
        inputattempt &add() { return *attempts.add(new inputattempt); }

        void copyto(inputrun &dst) const
        {
            dst.clear();
            loopv(attempts) attempts[i]->copyto(dst.add());
            loopv(tells) { runchunk *c = dst.tells.add(new runchunk); memcpy(c->type, tells[i]->type, sizeof(c->type)); c->late = tells[i]->late; c->data = tells[i]->data; }
        }

        void moveto(inputrun &dst)
        {
            dst.clear();
            loopv(attempts) dst.attempts.add(attempts[i]);
            attempts.setsize(0);
            loopv(tells) dst.tells.add(tells[i]);
            tells.setsize(0);
        }
    };

    // Metadata stored at the start of a replay file.
    struct replayinfo
    {
        int version;    // of the container
        int simversion; // compared with REPLAYSIMVERSION: older replays will likely desync
        uint date;
        string client, player, map;
        int mode, flagrun, race, splices, finished, runtime, ticks, maps;
        int started; // the run began (flagrun: flag taken, race: 200 ticks); -1 unknown (the oldest replays)
        bool live;   // describes the run being played now, not one read from a file

        replayinfo() : version(0), simversion(0), date(0), mode(0), flagrun(0), race(0), splices(0), finished(0), runtime(0), ticks(0), maps(0), started(-1), live(false)
        {
            client[0] = player[0] = map[0] = '\0';
        }
    };

    // tells.cpp: session and run counters for the replay file
    extern void tellsrunstart();
    extern void tellssave();
    extern void tellsload();
    extern void tellsreset();
    extern int tellsframe(int us);
    extern void tellslifestart();
    // the run (or life) being saved was slowed, sped up, paused, played with fixedframe or with
    // single-player slow motion (which also keeps the player alive)
    extern bool tellsassisted(const inputrun &r);
    extern bool tellsassistframe(int features);

    // sauer_resim -V: check a life's saved start against a fresh map start (savestate.cpp)
    extern bool replaylifecheck;
    extern string replaylifecheckresult;
    extern void lifestartfresh();
    extern void lifestartcompare(bool checkrng, uint rngseed, char *out, int outlen);
    extern bool replayrules(const char *file, const char *keys, int &fails, int &warns, int &missing, char *failed);
    extern int inputrunaids(const inputrun &r);
    extern void tellssavinglife(bool on); // the replay being saved holds one life
    // spawns.cpp: a custom flagrun spawn, or a forced race start, is in use
    extern bool spawnaidsactive(bool &custom, bool &race);
    extern int tellsrunms();
    extern long long tellsrunus();
    // a lasting copy of a bind action for raw events; the same text gives the same pointer
    extern const char *internbind(const char *s, int len);
    extern void tellsencode(const inputrun &run, const replayinfo &info, vector<runchunk *> &out);

    // content, if given, receives an SBR2 file's content hash (32 bytes)
    extern bool writereplay(const char *file, const inputrun &run, const replayinfo &info, uchar *content = NULL);
    extern void fillreplayinfo(const inputrun &run, bool finished, int runtime, replayinfo &info);

    // online witnessing (replayfile.cpp)
    extern void witnessupdate();
    extern void witnessfromhub(const char *host, int port, const uchar *channel, const uchar *signing);
    // the hub (hub.cpp)
    extern void hubupdate();
    extern bool witnessseed(uint &seed);
    extern void witnessrunstart(const inputattempt &header, const inputattempt &data, int skiptick, bool life, int continues = -1);
    extern int witnesstakefinished();
    extern void witnessdrop(const char *reason);
    extern void witnessnextattempt(const inputattempt &a);
    extern void witnessframe(int curtick);
    // the spectator stream to the hub (replayfile.cpp; hub.cpp says when it is wanted)
    extern bool hubstreaming();
    extern const char *hubrunkind();
    extern bool hubviewing();
    extern int hubviewstart();
    extern void hubviewstop();
    extern void hubviewbegin(const inputattempt &header, const inputattempt &data, int skiptick);
    extern void hubviewnextattempt(const inputattempt &a);
    extern void hubviewframe(int curtick);
    extern void witnessstop(const char *reason);
    extern int replaylongestidle(const inputrun &run);
    extern bool readreplay(const char *file, inputrun *run, replayinfo &info);
    extern bool readreplayprefix(const char *file, inputrun &run, replayinfo &info, int &known);
    // content: the replay's SBR2 content hash (32 bytes), or NULL
    extern void statsrecord(const inputrun &run, const replayinfo &info, const char *replay, const uchar *content = NULL);
    extern void statsreplaymoved(const char *from, const char *to);
    extern bool statspbreplay(const char *map, const char *kind, int mode, bool segmented, char *file);
    extern void statspbreplays(vector<char *> &files);
    extern void statsassistedreplays(vector<char *> &files);
    extern int statsversion();
    // head of the chain over all recorded attempts (32 bytes), its length, and the attempt count
    extern bool statshistory(uchar *head, int &count, int &attempts);
    // the ghost chosen for a map, kind and mode: source "pb", "segpb" or "file"; label may be empty
    extern bool statsghostchoice(const char *map, const char *kind, int mode, char *source, char *file, char *label);
    extern void statssetghost(const char *map, const char *kind, int mode, const char *source, const char *file, const char *label);

    // a split of the current run: name and run timer tick
    struct splittime
    {
        string name;
        int tick;
    };
    struct splitsegment
    {
        string prev, name;
        int ticks;
    };
    extern void splitresults(vector<splittime> &out);
    extern bool statssplitsofreplay(const char *file, vector<splittime> &out);
    // the chosen ghost when it is a replay rather than a PB: its file, samples (with the attempt
    // index of each) and run time in ms (-1 unfinished)
    extern bool ghostchosenreplay(const char *&file, const vector<ghostsample> *&samples, const vector<int> *&maps, int &runtime);
    extern const char *ghostname();
    extern bool statssplitcompare(const char *map, const char *kind, int mode, bool segmented, vector<splittime> &pb, vector<splitsegment> &best, int &pbms);
    extern const inputattempt *inputrunfirst();
    extern const char *inputwatchedfile();
    extern void ghostattach(inputrun &run);
    // player1 as a ghost sample at tick; lobby members' samples from the hub (hub.cpp)
    extern void sampleplayer(ghostsample &g, int tick);
    extern void lobbyghostsample(uint id, const char *name, const ghostsample &s);
    extern void lobbyghostsclear();
    // spectating through the hub (spectate.cpp)
    extern bool spectating();
    extern uint spectatingid();
    extern const char *spectatingname();
    extern bool spectatehold();
    extern float spectatebehind();
    extern void spectatebegin(uint target, const char *name, const char *map, int mode, const char *kind);
    extern void spectaterun(uint run, const char *map, int mode, bool chained);
    extern void spectatedata(uint run, int offset, const uchar *data, int len);
    extern void spectateend(const char *why);
    extern void spectatestop();
    extern void spectateupdate();
    // file: the replay file it was read from, or NULL
    extern void playinputrun(const inputrun &run, const char *file = NULL);
    extern bool inputreplayactive();
    extern bool (*replaywaitattempt)();
    extern void inputreplayextend(const inputrun &src, bool complete, bool appendonly);
    extern bool inputreplayneeds(int known);
    extern int inputreplayattempts();
    extern void (*replaychainhook)();
    extern bool inputreplaycontinue(const inputrun &src);
    extern bool inputreplayswitch(const inputrun &src);
    extern int inputreplayresult(int &ticks, uint hash[DETHASH_GROUPS]);
    extern int runtimerresult(bool &finished);
}

#endif
