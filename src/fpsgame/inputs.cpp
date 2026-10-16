// Offline input stream. Live input that arrives between ticks is queued and applied at the
// start of the next tick, and everything the simulation reads from the player is recorded
// per tick, so the current attempt can be replayed exactly.

#include "game.h"

extern int unescapestring(char *dst, const char *src, const char *end);
extern int zoom;

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

    struct tickhash { uint groups[DETHASH_GROUPS]; };

    // One attempt: everything needed to replay it from its map start, plus the state hash
    // at the start of every tick to find where a replay drifts.
    struct inputattempt
    {
        vector<inputevent> events;
        vector<tickhash> hashes;
        uint seed;
        string map, team;
        int mode;

        inputattempt() : seed(0), mode(0) { map[0] = team[0] = '\0'; }
        ~inputattempt() { clear(); }

        void clear()
        {
            loopv(events) DELETEA(events[i].cmd);
            events.setsize(0);
            hashes.setsize(0);
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
            dst.seed = seed;
            copystring(dst.map, map);
            copystring(dst.team, team);
            dst.mode = mode;
        }

        void moveto(inputattempt &dst)
        {
            dst.clear();
            loopv(events) dst.events.add(events[i]);
            events.setsize(0);
            dst.hashes = hashes;
            hashes.setsize(0);
            dst.seed = seed;
            copystring(dst.map, map);
            copystring(dst.team, team);
            dst.mode = mode;
        }
    };

    static vector<inputevent> pending;
    static inputattempt recording, previous, replaydata;
    static int curtick = 0, replaypos = 0, replaydesync = 0;
    static bool replaying = false, replaypending = false, havecontrols = false, recordingoff = false;
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
        }
    }

    static const char * const hashgroupnames[DETHASH_GROUPS] = { "player", "monsters", "projectiles", "movables", "entities", "rng" };

    static int replaydesyncgroups = 0;

    // Reports the first mismatching tick, then logs when each further group first differs.
    static void checkreplayhash(const tickhash &h)
    {
        if(curtick > replaydata.hashes.length()) return;
        const tickhash &want = replaydata.hashes[curtick-1];
        vector<char> diff;
        loopi(DETHASH_GROUPS) if(h.groups[i] != want.groups[i] && !(replaydesyncgroups & (1<<i)))
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

    void begintick()
    {
        if(!connected) { clearevents(pending); return; }
        curtick++;
        tickhash h;
        if(replaying && curtick > replaydata.hashes.length())
        {
            replaying = false;
            recordingoff = true;
            if(replaydesync) conoutf(CON_ERROR, "input replay finished (desynced at tick %d)", replaydesync);
            else conoutf("input replay finished");
        }
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
            while(replaypos < replaydata.events.length() && replaydata.events[replaypos].tick <= curtick)
            {
                applyevent(replaydata.events[replaypos++]);
                if(!replaying) return;
            }
            setcontrols(lastcontrols);
            hashgamestate(h.groups);
            checkreplayhash(h);
            return;
        }
        if(curtick == 1) copystring(recording.team, player1->team);
        while(pending.length())
        {
            inputevent e = pending.remove(0);
            e.tick = curtick;
            applyevent(e);
            if(curtick != e.tick) { DELETEA(e.cmd); return; }
            recording.events.add(e);
        }
        inputcontrols c;
        getcontrols(c);
        if(!havecontrols || c != lastcontrols)
        {
            inputevent &e = recording.events.add();
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
            inputevent &e = recording.events.add();
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
            inputevent &e = recording.events.add();
            e.tick = curtick;
            e.type = INPUT_ZOOM;
            e.arg = zoom;
            memset(&e.controls, 0, sizeof(e.controls));
            e.cursorx = e.cursory = 0;
            e.cmd = NULL;
        }
        lastzoom = zoom;
        hashgamestate(h.groups);
        recording.hashes.add(h);
    }

    void inputmapstart(const char *name, int mode)
    {
        clearevents(pending);
        curtick = 0;
        havecontrols = haveguistate = recordingoff = false;
        lastzoom = 0;
        if(replaypending)
        {
            replaypending = false;
            replaying = true;
            replaypos = 0;
            replaydesync = replaydesyncgroups = 0;
            gameseed = replaydata.seed;
            seedMT(gameseed);
            logoutf("game seed: %u (replay, %d events)", gameseed, replaydata.events.length());
            return;
        }
        if(replaying)
        {
            replaying = false;
            conoutf(replaydesync ? CON_ERROR : CON_INFO, "input replay ended%s", replaydesync ? " (desynced)" : "");
        }
        seedgame();
        if(!recording.empty()) recording.moveto(previous);
        recording.seed = gameseed;
        copystring(recording.map, name);
        recording.mode = mode;
    }

    static void startreplay(const inputattempt &src)
    {
        src.copyto(replaydata);
        server::forcenextteam(replaydata.team);
        replaypending = true;
        changemap(replaydata.map, replaydata.mode);
    }

    // 0 replays the current attempt, 1 the one before it
    ICOMMAND(inputreplay, "i", (int *which),
    {
        inputattempt &src = *which ? previous : recording;
        if(multiplayer(false) || src.empty()) { conoutf(CON_ERROR, "inputreplay: no recorded attempt"); return; }
        startreplay(src);
    });

    // Debug text dump of an attempt (0 = current, 1 = previous), readable by inputload.
    ICOMMAND(inputdump, "si", (char *file, int *which),
    {
        inputattempt &a = *which ? previous : recording;
        if(a.empty()) { conoutf(CON_ERROR, "inputdump: no recorded attempt"); return; }
        stream *f = openutf8file(path(file, true), "w");
        if(!f) { conoutf(CON_ERROR, "inputdump: could not open %s", file); return; }
        f->printf("attempt 1 %u %d %s %s\n", a.seed, a.mode, escapestring(a.map), escapestring(a.team));
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
            f->printf("h %u %u %u %u %u %u\n", h.groups[0], h.groups[1], h.groups[2], h.groups[3], h.groups[4], h.groups[5]);
        }
        delete f;
        conoutf("inputdump: wrote %d events, %d ticks to %s", a.events.length(), a.hashes.length(), file);
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
        stream *f = openutf8file(path(file, true), "r");
        if(!f) { conoutf(CON_ERROR, "inputload: could not open %s", file); return; }
        inputattempt &a = previous;
        a.clear();
        char line[4096];
        bool ok = false;
        while(f->getline(line, sizeof(line)))
        {
            vector<char *> w;
            explodelist(line, w);
            if(w.length() >= 6 && !strcmp(w[0], "attempt"))
            {
                a.seed = uint(strtoul(w[2], NULL, 10));
                a.mode = parseint(w[3]);
                copyunescaped(a.map, w[4]);
                copyunescaped(a.team, w[5]);
                ok = true;
            }
            else if(w.length() >= 13 && !strcmp(w[0], "e"))
            {
                inputevent &e = a.events.add();
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
            else if(w.length() >= 7 && !strcmp(w[0], "h"))
            {
                tickhash &h = a.hashes.add();
                loopi(DETHASH_GROUPS) h.groups[i] = uint(strtoul(w[i+1], NULL, 10));
            }
            w.deletearrays();
        }
        delete f;
        if(!ok || a.empty()) { conoutf(CON_ERROR, "inputload: %s is not an input dump", file); a.clear(); return; }
        conoutf("inputload: %d events, %d ticks on %s", a.events.length(), a.hashes.length(), a.map);
        startreplay(a);
    });

    ICOMMAND(inputevents, "", (), intret(replaying ? replaydata.events.length() : recording.events.length()));
    ICOMMAND(inputdesync, "", (), intret(replaydesync));
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
