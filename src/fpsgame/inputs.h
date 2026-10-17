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
        uint seed, mapcrc;
        string map, team;
        int mode, flagrun, race, ticks;
        int hashgroups; // groups stored per hash: DETHASH_LEGACYGROUPS for replays made before v4

        inputattempt() : starttick(0), seed(0), mapcrc(0), mode(0), flagrun(0), race(0), ticks(0), hashgroups(DETHASH_GROUPS) { map[0] = team[0] = '\0'; }
        ~inputattempt() { clear(); }

        void clear()
        {
            loopv(events) DELETEA(events[i].cmd);
            events.setsize(0);
            hashes.setsize(0);
            settings.setsize(0);
            snapshot.setsize(0);
            starttick = 0;
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
            dst.seed = seed;
            dst.mapcrc = mapcrc;
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
        int version;
        uint date;
        string client, player, map;
        int mode, flagrun, race, finished, runtime, ticks, maps;

        replayinfo() : version(0), date(0), mode(0), flagrun(0), race(0), finished(0), runtime(0), ticks(0), maps(0)
        {
            client[0] = player[0] = map[0] = '\0';
        }
    };

    extern bool writereplay(const char *file, const inputrun &run, const replayinfo &info);
    extern void fillreplayinfo(const inputrun &run, bool finished, int runtime, replayinfo &info);
    extern bool readreplay(const char *file, inputrun *run, replayinfo &info);
    extern void playinputrun(const inputrun &run);
    extern bool inputreplayactive();
    extern int inputreplayresult(int &ticks, uint hash[DETHASH_GROUPS]);
    extern int runtimerresult(bool &finished);
}

#endif
