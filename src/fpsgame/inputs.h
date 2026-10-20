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
    };

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
    struct segmenttiming
    {
        enum { BUCKETS = 8 };
        int segment;
        long long wallus, pausedus;
        int frames, frames0, frames2; // all frames, frames with no tick, with 2 or more ticks
        int hist[BUCKETS];            // frame times: <2, <4, <8, <16, <33, <66, <250, >=250 ms
        int speedmin, speedmax;       // gamespeed
        int fixedframe;               // highest fixedframe

        segmenttiming(int segment = 0) : segment(segment), wallus(0), pausedus(0), frames(0), frames0(0), frames2(0), speedmin(0), speedmax(0), fixedframe(0)
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
        vector<inputsetting> settings; // values at the start of the attempt; empty before v5
        vector<uchar> snapshot;          // saved state the attempt starts from instead of map start
        int starttick;                   // tick of the snapshot, 0 without one
        vector<inputsplice> splices;
        vector<ghostsample> trace; // v9+, finished or saved runs only
        vector<mapdatafile> files; // what the map load read; empty in SBRP files
        vector<segmenttiming> timing; // by segment, empty in SBRP files
        vector<rawevent> raw;         // binds point into internbind's pool; empty in SBRP files
        uint seed, mapcrc;
        uint cfgcrc;    // crc of the map cfg, 0 without one
        bool hascfgcrc; // false before v8
        string map, team;
        int mode, flagrun, race, ticks;
        int hashgroups; // groups stored per hash: DETHASH_LEGACYGROUPS for replays made before v4

        inputattempt() : starttick(0), seed(0), mapcrc(0), cfgcrc(0), hascfgcrc(false), mode(0), flagrun(0), race(0), ticks(0), hashgroups(DETHASH_GROUPS) { map[0] = team[0] = '\0'; }
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
            dst.seed = seed;
            dst.mapcrc = mapcrc;
            dst.cfgcrc = cfgcrc;
            dst.hascfgcrc = hascfgcrc;
            copystring(dst.map, map);
            copystring(dst.team, team);
            dst.mode = mode;
            dst.flagrun = flagrun;
            dst.race = race;
            dst.hashgroups = hashgroups;
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
        int format;     // 1 SBRP, 2 SBR2
        int version;    // of the format
        int simversion; // compared with REPLAYSIMVERSION: older replays will likely desync
        uint date;
        string client, player, map;
        int mode, flagrun, race, splices, finished, runtime, ticks, maps;
        int started; // the run began (flagrun: flag taken, race: 200 ticks); -1 unknown (before v10)
        bool live;   // describes the run being played now, not one read from a file

        replayinfo() : format(1), version(0), simversion(0), date(0), mode(0), flagrun(0), race(0), splices(0), finished(0), runtime(0), ticks(0), maps(0), started(-1), live(false)
        {
            client[0] = player[0] = map[0] = '\0';
        }
    };

    // tells.cpp: session and run counters for the replay file
    extern void tellsrunstart();
    extern void tellssave();
    extern void tellsload();
    extern void tellsreset();
    extern void tellsframe(int us);
    extern int tellsrunms();
    extern long long tellsrunus();
    // a lasting copy of a bind action for raw events; the same text gives the same pointer
    extern const char *internbind(const char *s, int len);
    extern void tellsencode(const inputrun &run, const replayinfo &info, vector<runchunk *> &out);

    // content, if given, receives an SBR2 file's content hash (32 bytes)
    extern bool writereplay(const char *file, const inputrun &run, const replayinfo &info, uchar *content = NULL);
    extern void fillreplayinfo(const inputrun &run, bool finished, int runtime, replayinfo &info);
    extern int replaylongestidle(const inputrun &run);
    extern bool readreplay(const char *file, inputrun *run, replayinfo &info);
    // content: the replay's SBR2 content hash (32 bytes), or NULL
    extern void statsrecord(const inputrun &run, const replayinfo &info, const char *replay, const uchar *content = NULL);
    extern void statsreplaymoved(const char *from, const char *to);
    extern bool statspbreplay(const char *map, const char *kind, int mode, bool segmented, char *file);
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
    // file: the replay file it was read from, or NULL
    extern void playinputrun(const inputrun &run, const char *file = NULL);
    extern bool inputreplayactive();
    extern int inputreplayresult(int &ticks, uint hash[DETHASH_GROUPS]);
    extern int runtimerresult(bool &finished);
}

#endif
