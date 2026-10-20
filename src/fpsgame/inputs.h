// Recorded input of offline games (see inputs.cpp) and the replay file format.

#ifndef INPUTS_H
#define INPUTS_H

namespace game
{
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
        int tick, discarded;
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

        ~inputrun() { clear(); }
        void clear() { attempts.deletecontents(); }
        bool empty() const { return attempts.empty() || (attempts.length() == 1 && attempts[0]->empty()); }
        inputattempt &last() { return *attempts.last(); }
        inputattempt &add() { return *attempts.add(new inputattempt); }

        void copyto(inputrun &dst) const
        {
            dst.clear();
            loopv(attempts) attempts[i]->copyto(dst.add());
        }

        void moveto(inputrun &dst)
        {
            dst.clear();
            loopv(attempts) dst.attempts.add(attempts[i]);
            attempts.setsize(0);
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

        replayinfo() : format(1), version(0), simversion(0), date(0), mode(0), flagrun(0), race(0), splices(0), finished(0), runtime(0), ticks(0), maps(0), started(-1)
        {
            client[0] = player[0] = map[0] = '\0';
        }
    };

    extern bool writereplay(const char *file, const inputrun &run, const replayinfo &info);
    extern void fillreplayinfo(const inputrun &run, bool finished, int runtime, replayinfo &info);
    extern int replaylongestidle(const inputrun &run);
    extern bool readreplay(const char *file, inputrun *run, replayinfo &info);
    extern void statsrecord(const inputrun &run, const replayinfo &info, const char *replay);
    extern void statsreplaymoved(const char *from, const char *to);
    extern bool statspbreplay(const char *map, const char *kind, int mode, bool segmented, char *file);
    extern int statsversion();
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
