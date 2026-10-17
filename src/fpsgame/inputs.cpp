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
    static int lastactivetick = 0, resetarmtick = -1;
    static int replayedticks = 0;
    static uint lastreplayhash[DETHASH_GROUPS];

    static void checkpendingreset();

    // movement, jump or attack input from the player (aim alone does not count)
    static void noteactivity()
    {
        if(player1->move || player1->strafe || player1->vertical || player1->jumping || player1->attacking) lastactivetick = curtick;
    }

    extern void saverunreplay(const inputrun &run, bool finished, int runtime);

    // Writes the current run as unfinished, once, unless it is a replay or was saved finished.
    static void saveunfinishedrun()
    {
        if(replaying || recordingoff || runsaved || recording.empty()) return;
        saverunreplay(recording, false, 0);
        runsaved = true;
    }

    void inputrunfinished(int runtime)
    {
        if(replaying || recordingoff || recording.empty()) return;
        saverunreplay(recording, true, runtime);
        runsaved = true;
    }

    void inputdisconnect() { saveunfinishedrun(); }
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

    int inputtick() { return curtick; }

    // A map change started by the map's own script during play continues a singleplayer run.
    bool mapchained(int mode)
    {
        return insimtick && (identflags&IDF_OVERRIDDEN) && m_check(mode, M_DMSP | M_CLASSICSP);
    }

    static inputattempt &replaycur() { return *replaydata.attempts[replayattempt]; }

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

    void begintick()
    {
        if(!connected) { clearevents(pending); return; }
        curtick++;
        tickhash h;
        if(replaying && curtick > replaycur().ticks)
        {
            replaying = false;
            recordingoff = true;
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
            while(replaypos < a.events.length() && a.events[replaypos].tick <= curtick)
            {
                int attempt = replayattempt;
                applyevent(a.events[replaypos++]);
                if(!replaying || attempt != replayattempt || !curtick) return;
            }
            setcontrols(lastcontrols);
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
        }
        while(pending.length())
        {
            inputevent e = pending.remove(0);
            e.tick = curtick;
            applyevent(e);
            if(curtick != e.tick) { DELETEA(e.cmd); return; }
            rec.events.add(e);
        }
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
            return;
        }
        if(replaying)
        {
            if(chained && replayattempt+1 < replaydata.attempts.length() && !strcmp(replaydata.attempts[replayattempt+1]->map, name))
            {
                replayattempt++;
                seedattempt(replaycur());
                return;
            }
            replaying = false;
            conoutf(replaydesync ? CON_ERROR : CON_INFO, "input replay ended%s", replaydesync ? " (desynced)" : "");
            chained = false;
        }
        seedgame();
        if(!chained && !recording.empty()) recording.moveto(previous);
        if(!chained) { recording.clear(); runsaved = false; }
        inputattempt &a = recording.add();
        a.seed = gameseed;
        copystring(a.map, name);
        a.mode = mode;
        a.flagrun = server::flagrunactive() ? 1 : 0;
        a.race = server::racerulesactive() ? 1 : 0;
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

    // 0 replays the current run, 1 the one before it
    ICOMMAND(inputreplay, "i", (int *which),
    {
        inputrun &src = *which ? previous : recording;
        if(multiplayer(false) || src.empty()) { conoutf(CON_ERROR, "inputreplay: no recorded run"); return; }
        playinputrun(src);
    });

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
