// Replay files: a run's recorded input plus metadata in SBR2 form (the container is in
// replayio.h, the run layout below), in replays/ (and replays/kept/ for runs protected from
// the rolling cleanup of unfinished attempts).

#include "game.h"
#include "inputs.h"
#include "replayio.h"
#include "blake3.h"

namespace game
{
    // SIMVERSION: the simulation's version, stored in every replay. REPLAYSIMVERSION: oldest
    // version the current simulation still plays back; bump both when the simulation changes.
    enum { SIMVERSION = 10, REPLAYSIMVERSION = 2, REPLAYHASHSTEP = 50 };
    // ghost path precision: positions in 1/GHOSTPOSSCALE units, angles in 1/GHOSTANGLESCALE degrees
    enum { GHOSTPOSSCALE = 4, GHOSTANGLESCALE = 2 };

    static int packcontrols(const inputcontrols &c)
    {
        return (c.move+1) | ((c.strafe+1)<<2) | ((c.vertical+1)<<4) | (c.jumping ? 1<<6 : 0) | (c.attacking ? 1<<7 : 0);
    }

    static void unpackcontrols(int v, inputcontrols &c)
    {
        c.move = (v&3)-1;
        c.strafe = ((v>>2)&3)-1;
        c.vertical = ((v>>4)&3)-1;
        c.jumping = (v>>6)&1;
        c.attacking = (v>>7)&1;
    }

    // SBR2 files (the container is in replayio.h):
    //   META   key-value pairs, keys sorted: client, date, flagrun, map, maps, mode, player, race
    //   DATA   var sim version, var files, per file: u8 role, str path, var size, BLAKE3
    //   run metadata (see tells.cpp), key-value: cLNT, sESS, hIST, pROV, SETS
    //   bIND   (optional) var binds, str per bind: the bind actions rAWI refers to
    //   per attempt:
    //     ATTM var index, str map, u32 map crc, u8 has cfg crc, u32 cfg crc, u32 seed, str team,
    //          svar mode, var flagrun, var race, var start tick, var ticks, var hash groups,
    //          var files, var DATA index per file, var settings, (str name, svar value) per
    //          setting, var saved state length, saved state
    //     per segment of SEGMENTTICKS ticks [lo, hi), by absolute tick:
    //     Lists are stored as columns: each field of every entry, then the next field.
    //       INPS var lo, var hi-lo, var events; columns: var tick delta (from the previous event
    //            or lo), u8 type, u8 source (INPUTSRC_*); for the controls events: svar yaw,
    //            svar pitch (float bits minus those of the chunk's previous controls event, or
    //            of 0), u8 packed move/strafe/vertical/jumping/attacking; then the other events'
    //            bodies in order: jump, attack, zoom = svar; command = var id in replaycommands,
    //            str arguments; GUI = u32 cursor x, u32 cursor y, str GUI names; setting = svar
    //            value, str name; map GUI action = str script
    //       rAWI (optional) var base time (in RAWTIMEUNIT µs), var base SDL ms, var events;
    //            columns: svar time delta (from the previous event or the base), svar SDL ms
    //            minus base ms minus floor(time since base / 1 ms), svar tick delta (from the
    //            previous event or lo), u8 kind; for the motion events: svar dx, svar dy,
    //            u8 flags; then the other events' bodies in order: key = var scancode,
    //            var keycode, u8 flags, var bind+1; button = u8 button, var bind+1; wheel =
    //            svar x, svar y, var bind+1; focus = u8 flags
    //       HASH var hashes; columns: var tick delta (from the previous hash or lo), u32
    //            combinedhash(); then the groups of the last hash, u32 each
    //       SPLC (only with state loads) var loads, (var tick, var discarded ticks, u8 kind,
    //            svar wall ms since the run began) per load
    //       tIME (optional) var wall us, var paused us, var frames, var frames without a tick,
    //            var frames with 2+ ticks, 8 x var frame-time histogram, var min gamespeed,
    //            var max gamespeed, var max fixedframe
    //       SEGE
    //     gHST (optional) var samples; columns: var run tick delta; svar x, y, z in
    //          1/GHOSTPOSSCALE units and svar yaw, pitch in 1/GHOSTANGLESCALE degrees (each minus
    //          the previous sample's); var flags
    //   RSLT   u8 finished, u8 started (2 unknown), var ticks, var runtime ms, var splices
    //   wIND, rSTA  key-value window metadata, raw input statistics
    //   END
    enum { SEGMENTTICKS = REPLAYSEGMENTTICKS };

    static void kvkey(sbr2::byteswriter &w, const char *key, int type) { w.str(key); w.u8(type); }

    // game text (the game's own 8-bit encoding) as a UTF-8 str
    static void utf8str(sbr2::byteswriter &w, const char *s)
    {
        uchar buf[4*MAXSTRLEN];
        size_t n = encodeutf8(buf, sizeof(buf), (const uchar *)s, strlen(s));
        w.var(n);
        w.bytes(buf, int(n));
    }

    static void fromutf8(char *dst, const char *src)
    {
        size_t n = decodeutf8((uchar *)dst, MAXSTRLEN-1, (const uchar *)src, strlen(src));
        dst[n] = '\0';
    }

    // a ghost path coordinate in fixed point
    static int quantize(float v, float scale) { return int(clamp(roundf(v*scale), -1e9f, 1e9f)); }

    static int datafileindex(const vector<const mapdatafile *> &files, const mapdatafile &f)
    {
        loopv(files) if(files[i]->role == f.role && files[i]->size == f.size && !strcmp(files[i]->path, f.path) && !memcmp(files[i]->hash, f.hash, sizeof(f.hash))) return i;
        return -1;
    }

    // The commands an SBR2 file can hold, by id: never reorder or remove. Id 0 holds any other
    // command, which playback refuses.
    static const char * const replaycommands[] =
    {
        "", "respawn", "respawnnow", "speedrunrespawn", "suicide", "taunt", "dropflag", "setspawnat", "clearspawnat",
        "nextweapon", "setweapon", "weapon", "cycleweapon", "addbot", "delbot", "botlimit", "botbalance",
        "cleargui", "clearmapguis"
    };

    // arguments that can't run other commands or read variables
    static bool plainargs(const char *s, int len)
    {
        loopi(len) if(!isalnum(uchar(s[i])) && !strchr(" \".-+_", s[i])) return false;
        return true;
    }

    static void writecommand(sbr2::byteswriter &w, const char *cmd)
    {
        const char *space = strchr(cmd, ' ');
        int namelen = space ? int(space - cmd) : int(strlen(cmd));
        for(int id = 1; id < int(sizeof(replaycommands)/sizeof(replaycommands[0])); id++)
        {
            if(int(strlen(replaycommands[id])) != namelen || strncmp(cmd, replaycommands[id], namelen)) continue;
            const char *args = space ? space + 1 : "";
            if(space && !*args) break;
            if(!plainargs(args, strlen(args))) break;
            w.var(id);
            w.str(args);
            return;
        }
        w.var(0);
        w.str(cmd);
    }

    static int bindindex(const vector<const char *> &binds, const char *bind)
    {
        if(bind) loopv(binds) if(binds[i] == bind) return i + 1;
        return 0;
    }

    // Events in columns: tick deltas, types, sources, then yaw, pitch and packed keys of the
    // controls events, then the other events' bodies in order.
    static void writeevents(sbr2::byteswriter &w, const inputevent *evs, int n, int lo)
    {
        vector<uchar> ticks, types, sources, yaws, pitches, keys, bodies;
        sbr2::byteswriter wt(ticks), wy(yaws), wp(pitches), wb(bodies);
        int prev = lo;
        uint yaw = 0, pitch = 0;
        loopi(n)
        {
            const inputevent &e = evs[i];
            wt.var(e.tick - prev);
            prev = e.tick;
            types.add(uchar(e.type));
            sources.add(uchar(e.source));
            switch(e.type)
            {
                case INPUT_CONTROLS:
                    wy.svar(int(e.controls.yaw - yaw));
                    wp.svar(int(e.controls.pitch - pitch));
                    keys.add(uchar(packcontrols(e.controls)));
                    yaw = e.controls.yaw;
                    pitch = e.controls.pitch;
                    break;
                case INPUT_COMMAND: writecommand(wb, e.cmd ? e.cmd : ""); break;
                case INPUT_MAPGUI: wb.str(e.cmd ? e.cmd : ""); break;
                case INPUT_GUI: wb.u32(e.cursorx); wb.u32(e.cursory); wb.str(e.cmd ? e.cmd : ""); break;
                case INPUT_SETTING: wb.svar(e.arg); wb.str(e.cmd ? e.cmd : ""); break;
                default: wb.svar(e.arg); break;
            }
        }
        w.var(n);
        const vector<uchar> *cols[] = { &ticks, &types, &sources, &yaws, &pitches, &keys, &bodies };
        loopi(sizeof(cols)/sizeof(cols[0])) w.bytes(cols[i]->getbuf(), cols[i]->length());
    }

    enum { RAWTIMEUNIT = 100 }; // raw event times are stored in units of this many µs

    static long long floordiv(long long a, long long b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

    // Raw events in columns: time deltas (RAWTIMEUNIT), SDL ms as an offset from the time
    // column, tick deltas, kinds, then dx, dy and flags of the motion events, then the other
    // events' bodies in order.
    static void writeraw(sbr2::byteswriter &w, const rawevent *evs, int n, int lo, const vector<const char *> &binds)
    {
        vector<uchar> us, ms, ticks, kinds, dxs, dys, flags, bodies;
        sbr2::byteswriter wus(us), wms(ms), wt(ticks), wx(dxs), wy(dys), wb(bodies);
        long long base = max(evs[0].us, 0LL) / RAWTIMEUNIT, last = base;
        uint basems = evs[0].sdlms;
        int lasttick = lo;
        loopi(n)
        {
            const rawevent &e = evs[i];
            long long t = max(e.us, 0LL) / RAWTIMEUNIT;
            wus.svar(t - last);
            wms.svar(int(e.sdlms - basems) - floordiv(t - base, 1000 / RAWTIMEUNIT));
            wt.svar(e.tick - lasttick);
            last = t;
            lasttick = e.tick;
            kinds.add(uchar(e.kind));
            switch(e.kind)
            {
                case rawevent::KEYDOWN: case rawevent::KEYUP: wb.var(e.code); wb.var(uint(e.keycode)); wb.u8(e.flags); wb.var(bindindex(binds, e.bind)); break;
                case rawevent::BUTTONDOWN: case rawevent::BUTTONUP: wb.u8(e.code); wb.var(bindindex(binds, e.bind)); break;
                case rawevent::MOTION: wx.svar(e.dx); wy.svar(e.dy); flags.add(uchar(e.flags)); break;
                case rawevent::WHEEL: wb.svar(e.dx); wb.svar(e.dy); wb.var(bindindex(binds, e.bind)); break;
                case rawevent::FOCUS: wb.u8(e.flags); break;
            }
        }
        w.var(base);
        w.var(basems);
        w.var(n);
        const vector<uchar> *cols[] = { &us, &ms, &ticks, &kinds, &dxs, &dys, &flags, &bodies };
        loopi(sizeof(cols)/sizeof(cols[0])) w.bytes(cols[i]->getbuf(), cols[i]->length());
    }

    // State hashes: tick deltas, the combined hash of each, then every group of the last one.
    static void writehashes(sbr2::byteswriter &w, const tickhash * const *hashes, int n, int lo, int groups)
    {
        w.var(n);
        int prev = lo;
        loopi(n) { w.var(hashes[i]->tick - prev); prev = hashes[i]->tick; }
        loopi(n) w.u32(combinedhash(hashes[i]->groups, groups));
        if(n) loopi(groups) w.u32(hashes[n-1]->groups[i]);
    }

    static void writeattempt(sbr2::writer &out, int index, const inputattempt &a, const vector<const mapdatafile *> &files, const vector<const char *> &binds)
    {
        vector<uchar> p;
        sbr2::byteswriter w(p);
        w.var(index);
        w.str(a.map);
        w.u32(a.mapcrc);
        w.u8(a.hascfgcrc ? 1 : 0);
        w.u32(a.cfgcrc);
        w.u32(a.seed);
        w.str(a.team);
        w.svar(a.mode);
        w.var(a.flagrun);
        w.var(a.race);
        w.var(a.starttick);
        w.var(a.ticks);
        w.var(a.hashgroups);
        w.var(a.files.length());
        loopv(a.files) w.var(datafileindex(files, a.files[i]));
        w.var(a.settings.length());
        loopv(a.settings) { w.str(a.settings[i].name); w.svar(a.settings[i].value); }
        w.var(a.snapshot.length());
        w.bytes(a.snapshot.getbuf(), a.snapshot.length());
        out.add("ATTM", p, true);

        vector<const tickhash *> hashes;
        loopv(a.hashes) if(a.hashes[i].tick % REPLAYHASHSTEP == 0 || a.hashes[i].tick == a.ticks) hashes.add(&a.hashes[i]);
        int first = a.starttick, last = a.ticks;
        if(a.events.length()) { first = min(first, a.events[0].tick); last = max(last, a.events[a.events.length()-1].tick); }
        if(hashes.length()) { first = min(first, hashes[0]->tick); last = max(last, hashes.last()->tick); }
        if(a.splices.length()) { first = min(first, a.splices[0].tick); last = max(last, a.splices[a.splices.length()-1].tick); }
        if(a.raw.length()) { first = min(first, a.raw[0].tick); last = max(last, a.raw[a.raw.length()-1].tick); }
        int ev = 0, hs = 0, sp = 0, tm = 0, rw = 0;
        for(int seg = max(first, 0)/SEGMENTTICKS; seg <= last/SEGMENTTICKS; seg++)
        {
            int lo = seg*SEGMENTTICKS, hi = lo + SEGMENTTICKS;
            int n = 0;
            while(ev + n < a.events.length() && a.events[ev + n].tick < hi) n++;
            p.setsize(0);
            w.var(lo);
            w.var(hi - lo);
            writeevents(w, a.events.getbuf() + ev, n, lo);
            ev += n;
            out.add("INPS", p, true);

            n = 0;
            while(rw + n < a.raw.length() && a.raw[rw + n].tick < hi) n++;
            if(n)
            {
                p.setsize(0);
                writeraw(w, &a.raw[rw], n, lo, binds);
                rw += n;
                out.add("rAWI", p, true);
            }

            n = 0;
            while(hs + n < hashes.length() && hashes[hs + n]->tick < hi) n++;
            p.setsize(0);
            writehashes(w, hashes.getbuf() + hs, n, lo, a.hashgroups);
            hs += n;
            out.add("HASH", p);

            n = 0;
            while(sp + n < a.splices.length() && a.splices[sp + n].tick < hi) n++;
            if(n)
            {
                p.setsize(0);
                w.var(n);
                loopi(n)
                {
                    const inputsplice &s = a.splices[sp++];
                    w.var(s.tick);
                    w.var(s.discarded);
                    w.u8(s.kind);
                    w.svar(s.wallms);
                }
                out.add("SPLC", p);
            }

            while(tm < a.timing.length() && a.timing[tm].segment < seg) tm++;
            if(tm < a.timing.length() && a.timing[tm].segment == seg)
            {
                const segmenttiming &t = a.timing[tm];
                p.setsize(0);
                w.var(t.wallus);
                w.var(t.pausedus);
                w.var(t.frames);
                w.var(t.frames0);
                w.var(t.frames2);
                loopi(segmenttiming::BUCKETS) w.var(t.hist[i]);
                w.var(t.speedmin);
                w.var(t.speedmax);
                w.var(t.fixedframe);
                out.add("tIME", p, true);
            }
            out.endsegment(hi);
        }

        if(a.trace.length())
        {
            p.setsize(0);
            w.var(a.trace.length());
            int lasttick = 0;
            loopv(a.trace) { w.var(a.trace[i].tick - lasttick); lasttick = a.trace[i].tick; }
            loopj(5)
            {
                int last = 0;
                loopv(a.trace)
                {
                    const ghostsample &g = a.trace[i];
                    float v = j < 3 ? g.o[j] : j == 3 ? g.yaw : g.pitch;
                    int q = quantize(v, j < 3 ? GHOSTPOSSCALE : GHOSTANGLESCALE);
                    w.svar(q - last);
                    last = q;
                }
            }
            loopv(a.trace) w.var(a.trace[i].flags);
            out.add("gHST", p, true);
        }
    }

    static bool writesbr2(const char *file, const inputrun &run, const replayinfo &info, uchar *content = NULL)
    {
        sbr2::writer out;
        vector<uchar> p;
        sbr2::byteswriter w(p);
        w.var(8);
        kvkey(w, "client", sbr2::VALUE_STR); w.str(info.client);
        kvkey(w, "date", sbr2::VALUE_VAR); w.var(info.date);
        kvkey(w, "flagrun", sbr2::VALUE_VAR); w.var(info.flagrun);
        kvkey(w, "map", sbr2::VALUE_STR); utf8str(w, run.attempts.length() ? run.attempts[0]->map : "");
        kvkey(w, "maps", sbr2::VALUE_VAR); w.var(run.attempts.length());
        kvkey(w, "mode", sbr2::VALUE_SVAR); w.svar(info.mode);
        kvkey(w, "player", sbr2::VALUE_STR); utf8str(w, info.player);
        kvkey(w, "race", sbr2::VALUE_VAR); w.var(info.race);
        out.add("META", p);

        vector<const mapdatafile *> files;
        loopv(run.attempts) loopvj(run.attempts[i]->files)
        {
            const mapdatafile &f = run.attempts[i]->files[j];
            if(datafileindex(files, f) < 0) files.add(&f);
        }
        p.setsize(0);
        w.var(info.simversion);
        w.var(files.length());
        loopv(files)
        {
            w.u8(files[i]->role);
            w.str(files[i]->path);
            w.var(files[i]->size);
            w.bytes(files[i]->hash, sizeof(files[i]->hash));
        }
        out.add("DATA", p);

        vector<runchunk *> made;
        if(info.live) tellsencode(run, info, made);
        const vector<runchunk *> &tells = info.live ? made : run.tells;
        loopv(tells) if(!tells[i]->late) out.add(tells[i]->type, tells[i]->data);

        vector<const char *> binds;
        loopv(run.attempts) loopvj(run.attempts[i]->raw)
        {
            const char *bind = run.attempts[i]->raw[j].bind;
            if(bind && binds.find(bind) < 0) binds.add(bind);
        }
        if(binds.length())
        {
            p.setsize(0);
            w.var(binds.length());
            loopv(binds) w.str(binds[i]);
            out.add("bIND", p, true);
        }

        loopv(run.attempts) writeattempt(out, i, *run.attempts[i], files, binds);

        p.setsize(0);
        w.u8(info.finished ? 1 : 0);
        w.u8(info.started < 0 ? 2 : info.started ? 1 : 0);
        w.var(info.ticks);
        w.var(info.runtime);
        w.var(info.splices);
        out.add("RSLT", p);
        loopv(tells) if(tells[i]->late) out.add(tells[i]->type, tells[i]->data);
        made.deletecontents();
        out.finish();
        if(content) memcpy(content, out.content, sbr2::HASHSIZE);

        stream *f = openrawfile(path(file, true), "wb");
        if(!f) return false;
        bool ok = f->write(out.out.getbuf(), out.out.length()) == size_t(out.out.length());
        delete f;
        return ok;
    }

    static bool readmeta(sbr2::bytesreader &r, replayinfo &info)
    {
        int n = int(r.var());
        string prev = "";
        loopi(n)
        {
            string key, s = "";
            if(!r.str(key, sizeof(key)) || (i && strcmp(prev, key) >= 0)) return false;
            copystring(prev, key);
            int type = r.u8();
            long long sv = 0;
            unsigned long long uv = 0;
            switch(type)
            {
                case sbr2::VALUE_SVAR: sv = r.svar(); break;
                case sbr2::VALUE_VAR: uv = r.var(); break;
                case sbr2::VALUE_STR: case sbr2::VALUE_BYTES:
                {
                    unsigned long long len = r.var();
                    if(!r.ok || len > (unsigned long long)r.left()) return false;
                    if(type == sbr2::VALUE_STR) { int keep = min(int(len), int(sizeof(s)) - 1); memcpy(s, r.p, keep); s[keep] = '\0'; }
                    r.p += len;
                    break;
                }
                case sbr2::VALUE_F32: r.u32(); break;
                case sbr2::VALUE_BOOL: uv = r.u8(); if(uv > 1) return false; break;
                default: return false;
            }
            if(!r.ok) return false;
            if(type == sbr2::VALUE_STR && !strcmp(key, "client")) copystring(info.client, s);
            else if(type == sbr2::VALUE_VAR && !strcmp(key, "date")) info.date = uint(uv);
            else if(type == sbr2::VALUE_VAR && !strcmp(key, "flagrun")) info.flagrun = int(uv);
            else if(type == sbr2::VALUE_STR && !strcmp(key, "map")) fromutf8(info.map, s);
            else if(type == sbr2::VALUE_VAR && !strcmp(key, "maps")) info.maps = int(uv);
            else if(type == sbr2::VALUE_SVAR && !strcmp(key, "mode")) info.mode = int(sv);
            else if(type == sbr2::VALUE_STR && !strcmp(key, "player")) fromutf8(info.player, s);
            else if(type == sbr2::VALUE_VAR && !strcmp(key, "race")) info.race = int(uv);
        }
        return r.ok;
    }

    static bool readattempt(sbr2::bytesreader &r, inputattempt &a, const vector<mapdatafile> &files)
    {
        r.str(a.map, sizeof(a.map));
        a.mapcrc = r.u32();
        int hascfgcrc = r.u8();
        a.hascfgcrc = hascfgcrc != 0;
        a.cfgcrc = r.u32();
        a.seed = r.u32();
        r.str(a.team, sizeof(a.team));
        a.mode = int(r.svar());
        a.flagrun = int(r.var());
        a.race = int(r.var());
        a.starttick = int(r.var());
        a.ticks = int(r.var());
        a.hashgroups = int(r.var());
        if(!r.ok || hascfgcrc > 1 || a.hashgroups < 1 || a.hashgroups > DETHASH_GROUPS) return false;
        int n = int(r.var());
        loopi(n)
        {
            unsigned long long index = r.var();
            if(!r.ok || index >= (unsigned long long)files.length()) return false;
            a.files.add(files[int(index)]);
        }
        n = int(r.var());
        loopi(n)
        {
            if(!r.ok) return false;
            inputsetting &s = a.settings.add();
            r.str(s.name, sizeof(s.name));
            s.value = int(r.svar());
        }
        unsigned long long len = r.var();
        if(!r.ok || len > (unsigned long long)r.left()) return false;
        a.snapshot.put(r.p, int(len));
        r.p += len;
        return r.ok;
    }

    static bool readevents(sbr2::bytesreader &r, inputattempt &a, int &lo, int &hi)
    {
        lo = int(r.var());
        hi = lo + int(r.var());
        int n = int(r.var());
        if(!r.ok || n < 0 || n > r.left()) return false;
        int first = a.events.length(), tick = lo;
        loopi(n)
        {
            inputevent &e = a.events.add();
            memset(&e, 0, sizeof(e));
            tick += int(r.var());
            e.tick = tick;
            if(tick < lo || tick >= hi) return false;
        }
        loopi(n) a.events[first + i].type = r.u8();
        loopi(n) { inputevent &e = a.events[first + i]; e.source = r.u8(); if(e.source > INPUTSRC_MAP) return false; }
        if(!r.ok) return false;
        uint yaw = 0, pitch = 0;
        loopi(n) if(a.events[first + i].type == INPUT_CONTROLS) { yaw += uint(int(r.svar())); a.events[first + i].controls.yaw = yaw; }
        loopi(n) if(a.events[first + i].type == INPUT_CONTROLS) { pitch += uint(int(r.svar())); a.events[first + i].controls.pitch = pitch; }
        loopi(n) if(a.events[first + i].type == INPUT_CONTROLS) unpackcontrols(r.u8(), a.events[first + i].controls);
        loopi(n)
        {
            inputevent &e = a.events[first + i];
            if(!r.ok) return false;
            switch(e.type)
            {
                case INPUT_CONTROLS: break;
                case INPUT_COMMAND:
                {
                    unsigned long long id = r.var(), len = r.var();
                    if(!r.ok || len > (unsigned long long)r.left() || id >= sizeof(replaycommands)/sizeof(replaycommands[0])) return false;
                    const char *args = (const char *)r.p;
                    r.p += len;
                    if(!id) { e.cmd = newstring(args, size_t(len)); e.arg = 1; break; }
                    if(!plainargs(args, int(len))) return false;
                    vector<char> cmd;
                    cmd.put(replaycommands[id], strlen(replaycommands[id]));
                    if(len) { cmd.add(' '); cmd.put(args, int(len)); }
                    cmd.add('\0');
                    e.cmd = newstring(cmd.getbuf());
                    break;
                }
                case INPUT_MAPGUI: case INPUT_GUI: case INPUT_SETTING:
                {
                    if(e.type == INPUT_GUI) { e.cursorx = r.u32(); e.cursory = r.u32(); }
                    if(e.type == INPUT_SETTING) e.arg = int(r.svar());
                    unsigned long long len = r.var();
                    if(!r.ok || len > (unsigned long long)r.left()) return false;
                    e.cmd = newstring((const char *)r.p, size_t(len));
                    r.p += len;
                    break;
                }
                case INPUT_JUMP: case INPUT_ATTACK: case INPUT_ZOOM: e.arg = int(r.svar()); break;
                default: return false;
            }
        }
        return r.ok;
    }

    static bool readhashes(sbr2::bytesreader &r, inputattempt &a, int lo, int hi)
    {
        int n = int(r.var()), tick = lo, first = a.hashes.length();
        if(!r.ok || n < 0 || n > r.left()) return false;
        loopi(n)
        {
            tickhash &h = a.hashes.add();
            tick += int(r.var());
            h.tick = tick;
            h.combined = true;
            memset(h.groups, 0, sizeof(h.groups));
            if(tick < lo || tick >= hi) return false;
        }
        loopi(n) a.hashes[first + i].groups[0] = r.u32();
        if(n)
        {
            tickhash &last = a.hashes.last();
            uint combined = last.groups[0];
            loopj(a.hashgroups) last.groups[j] = r.u32();
            last.combined = false;
            if(combinedhash(last.groups, a.hashgroups) != combined) return false;
        }
        return r.ok;
    }

    static bool readsplices(sbr2::bytesreader &r, inputattempt &a, int lo, int hi)
    {
        int n = int(r.var());
        loopi(n)
        {
            if(!r.ok) return false;
            inputsplice &sp = a.splices.add();
            sp.tick = int(r.var());
            sp.discarded = int(r.var());
            sp.kind = r.u8();
            sp.wallms = int(r.svar());
            if(sp.kind > inputsplice::AUTO || sp.tick < lo || sp.tick >= hi || (a.splices.length() > 1 && sp.tick < a.splices[a.splices.length()-2].tick)) return false;
        }
        return r.ok && n > 0;
    }

    static bool readraw(sbr2::bytesreader &r, inputattempt &a, int lo, int hi, const vector<const char *> &binds)
    {
        long long base = (long long)r.var(), t = base;
        uint basems = uint(r.var());
        int n = int(r.var()), tick = lo, first = a.raw.length();
        if(!r.ok || n < 0 || n > r.left()) return false;
        loopi(n) { rawevent &e = a.raw.add(); t += r.svar(); e.us = t * RAWTIMEUNIT; }
        loopi(n)
        {
            rawevent &e = a.raw[first + i];
            e.sdlms = basems + uint(int(r.svar() + floordiv(e.us / RAWTIMEUNIT - base, 1000 / RAWTIMEUNIT)));
        }
        loopi(n)
        {
            tick += int(r.svar());
            if(tick < lo || tick >= hi) return false;
            a.raw[first + i].tick = tick;
        }
        loopi(n) a.raw[first + i].kind = r.u8();
        loopi(n) if(a.raw[first + i].kind == rawevent::MOTION) a.raw[first + i].dx = int(r.svar());
        loopi(n) if(a.raw[first + i].kind == rawevent::MOTION) a.raw[first + i].dy = int(r.svar());
        loopi(n) if(a.raw[first + i].kind == rawevent::MOTION) a.raw[first + i].flags = r.u8();
        loopi(n)
        {
            rawevent &e = a.raw[first + i];
            int bind = 0;
            switch(e.kind)
            {
                case rawevent::KEYDOWN: case rawevent::KEYUP: e.code = int(r.var()); e.keycode = int(uint(r.var())); e.flags = r.u8(); bind = int(r.var()); break;
                case rawevent::BUTTONDOWN: case rawevent::BUTTONUP: e.code = r.u8(); bind = int(r.var()); break;
                case rawevent::MOTION: break;
                case rawevent::WHEEL: e.dx = int(r.svar()); e.dy = int(r.svar()); bind = int(r.var()); break;
                case rawevent::FOCUS: e.flags = r.u8(); break;
                default: return false;
            }
            if(!r.ok || bind < 0 || bind > binds.length()) return false;
            e.bind = bind ? binds[bind-1] : NULL;
        }
        return r.ok;
    }

    static bool readtiming(sbr2::bytesreader &r, inputattempt &a, int segment)
    {
        segmenttiming &t = a.timing.add(segmenttiming(segment));
        t.wallus = (long long)r.var();
        t.pausedus = (long long)r.var();
        t.frames = int(r.var());
        t.frames0 = int(r.var());
        t.frames2 = int(r.var());
        loopi(segmenttiming::BUCKETS) t.hist[i] = int(r.var());
        t.speedmin = int(r.var());
        t.speedmax = int(r.var());
        t.fixedframe = int(r.var());
        return r.ok;
    }

    static bool readtrace(sbr2::bytesreader &r, inputattempt &a)
    {
        int n = int(r.var()), tick = 0, first = a.trace.length();
        if(!r.ok || n < 0 || n > r.left()) return false;
        loopi(n) { ghostsample &g = a.trace.add(); tick += int(r.var()); g.tick = tick; }
        loopj(5)
        {
            int q = 0;
            loopi(n)
            {
                ghostsample &g = a.trace[first + i];
                q += int(r.svar());
                if(j < 3) g.o[j] = q / float(GHOSTPOSSCALE);
                else if(j == 3) g.yaw = q / float(GHOSTANGLESCALE);
                else g.pitch = q / float(GHOSTANGLESCALE);
            }
        }
        loopi(n) a.trace[first + i].flags = int(r.var());
        return r.ok;
    }

    // With run == NULL the attempts are skipped, but the whole file is still checked.
    static bool readsbr2(const char *file, const vector<uchar> &buf, inputrun *run, replayinfo &info)
    {
        sbr2::reader rd;
        if(!rd.parse(buf.getbuf(), buf.length(), sbr2::KNOWNCHUNKS)) { conoutf(CON_ERROR, "replay %s: %s", file, rd.error); return false; }
        info = replayinfo();
        info.version = sbr2::VERSION;
        if(run) run->clear();
        vector<mapdatafile> files;
        bool meta = false, data = false, result = false;
        int attempts = 0, lo = 0, hi = 0, nexttick = 0, inpssegment = -1;
        inputattempt *a = NULL;
        vector<const char *> binds;
        loopv(rd.chunks)
        {
            const sbr2::chunk &c = *rd.chunks[i];
            sbr2::bytesreader r(c.data.getbuf(), c.data.length());
            bool ok = true, parsed = true;
            if(!memcmp(c.type, "META", 4)) { ok = !meta && i == 0 && readmeta(r, info); meta = true; }
            else if(!memcmp(c.type, "DATA", 4))
            {
                ok = meta && !data;
                data = true;
                info.simversion = int(r.var());
                int n = int(r.var());
                loopj(n)
                {
                    if(!r.ok) break;
                    mapdatafile &f = files.add();
                    f.role = r.u8();
                    r.str(f.path, sizeof(f.path));
                    f.size = uint(r.var());
                    r.bytes(f.hash, sizeof(f.hash));
                }
            }
            else if(!memcmp(c.type, "ATTM", 4))
            {
                ok = data && !result && int(r.var()) == attempts;
                attempts++;
                nexttick = 0;
                inpssegment = -1;
                a = run ? &run->add() : NULL;
                if(a && ok) ok = readattempt(r, *a, files);
                else parsed = false;
            }
            else if(!memcmp(c.type, "bIND", 4))
            {
                ok = data && !attempts && binds.empty();
                int n = int(r.var());
                loopj(n)
                {
                    unsigned long long len = r.var();
                    if(!r.ok || len > (unsigned long long)r.left()) { ok = false; break; }
                    binds.add(internbind((const char *)r.p, int(len)));
                    r.p += len;
                }
            }
            else if(!memcmp(c.type, "INPS", 4) || !memcmp(c.type, "HASH", 4) || !memcmp(c.type, "SPLC", 4) || !memcmp(c.type, "tIME", 4) || !memcmp(c.type, "rAWI", 4) || !memcmp(c.type, "gHST", 4))
            {
                ok = attempts > 0 && !result;
                if(!a) parsed = false;
                else if(!ok) {}
                else if(!memcmp(c.type, "INPS", 4))
                {
                    ok = inpssegment != c.segment && readevents(r, *a, lo, hi) && lo >= nexttick;
                    nexttick = hi;
                    inpssegment = c.segment;
                }
                else if(!memcmp(c.type, "gHST", 4)) ok = !a->trace.length() && readtrace(r, *a);
                else if(!memcmp(c.type, "tIME", 4)) ok = inpssegment == c.segment && readtiming(r, *a, lo / SEGMENTTICKS);
                else if(!memcmp(c.type, "rAWI", 4)) ok = inpssegment == c.segment && readraw(r, *a, lo, hi, binds);
                else
                {
                    ok = inpssegment == c.segment;
                    if(ok) ok = !memcmp(c.type, "HASH", 4) ? readhashes(r, *a, lo, hi) : readsplices(r, *a, lo, hi);
                }
            }
            else if(!memcmp(c.type, "RSLT", 4))
            {
                ok = attempts > 0 && !result;
                result = true;
                info.finished = r.u8();
                int started = r.u8();
                info.started = started == 2 ? -1 : started;
                info.ticks = int(r.var());
                info.runtime = int(r.var());
                info.splices = int(r.var());
                ok = ok && info.finished <= 1 && started <= 2;
            }
            else
            {
                parsed = false;
                if(run && memcmp(c.type, "SEGE", 4) && memcmp(c.type, "END ", 4) && memcmp(c.type, "sIGN", 4))
                {
                    runchunk *t = run->tells.add(new runchunk);
                    memcpy(t->type, c.type, sizeof(t->type));
                    t->late = result;
                    t->data = c.data;
                }
            }
            if(!ok || !r.ok || (parsed && r.left()))
            {
                conoutf(CON_ERROR, "replay %s: chunk %d (%s) is malformed or out of place", file, i, c.type);
                if(run) run->clear();
                return false;
            }
        }
        if(!meta || !data || !result || attempts != info.maps)
        {
            conoutf(CON_ERROR, "replay %s: incomplete", file);
            if(run) run->clear();
            return false;
        }
        return true;
    }

    bool writereplay(const char *file, const inputrun &run, const replayinfo &info, uchar *content)
    {
        return writesbr2(file, run, info, content);
    }

    // With run == NULL only the header is read.
    bool readreplay(const char *file, inputrun *run, replayinfo &info)
    {
        stream *f = openrawfile(path(file, true), "rb");
        if(!f) return false;
        vector<uchar> buf;
        uchar chunk[65536];
        for(size_t n; (n = f->read(chunk, sizeof(chunk))) > 0;) buf.put(chunk, int(n));
        delete f;
        if(buf.length() < 4 || memcmp(buf.getbuf(), "SBR2", 4) || !readsbr2(file, buf, run, info)) return false;
        if(run && info.simversion < REPLAYSIMVERSION) conoutf(CON_ERROR, "replay %s was made with an older build: it will likely desync", file);
        return true;
    }

    // replayverify: checks a replay for leaderboard use and prints one line per check
    struct verifier
    {
        int fails, warns;
        verifier() : fails(0), warns(0) {}

        void pass(const char *name, const char *detail = "") { conoutf("\f0PASS\f7 %s %s", name, detail); }
        void fail(const char *name, const char *detail = "") { conoutf("\f3FAIL\f7 %s %s", name, detail); fails++; }
        void warn(const char *name, const char *detail = "") { conoutf("\f2WARN\f7 %s %s", name, detail); warns++; }
        void check(bool ok, const char *name, const char *detail = "") { if(ok) pass(name, detail); else fail(name, detail); }
    };

    static const runchunk *findtells(const inputrun &run, const char *type)
    {
        loopv(run.tells) if(!memcmp(run.tells[i]->type, type, 4)) return run.tells[i];
        return NULL;
    }

    static long long tellvalue(const inputrun &run, const char *type, const char *key, long long missing)
    {
        const runchunk *c = findtells(run, type);
        long long v = missing;
        if(c) sbr2::kvfind(c->data, key, &v);
        return v;
    }

    // file: a list of hex public keys, one per line, or a single key in hex
    static void loadwitnesskeys(const char *file, vector<uchar> &keys)
    {
        uchar key[32];
        if(file && sbr2::fromhex(file, key, 32)) { keys.put(key, 32); return; }
        stream *f = openutf8file(path(file && file[0] ? file : "witnesskeys.txt", true), "r");
        if(!f) return;
        char line[512];
        while(f->getline(line, sizeof(line)))
        {
            char *s = line;
            while(*s && isspace(uchar(*s))) s++;
            char *e = s + strlen(s);
            while(e > s && isspace(uchar(e[-1]))) *--e = '\0';
            uchar key[32];
            if(sbr2::fromhex(s, key, 32)) keys.put(key, 32);
        }
        delete f;
    }

    static void verifysignatures(verifier &v, const sbr2::reader &rd, const char *keyfile)
    {
        vector<uchar> keys;
        loadwitnesskeys(keyfile, keys);
        int good = 0, unknown = 0, bad = 0, final = 0;
        loopv(rd.chunks)
        {
            const sbr2::chunk &c = *rd.chunks[i];
            if(memcmp(c.type, "sIGN", 4)) continue;
            sbr2::signature s;
            if(!s.read(c.data.getbuf(), c.data.length())) { bad++; continue; }
            const uchar *expected = s.scope == sbr2::signature::FINAL ? rd.content :
                                    s.segment < rd.heads.length() / sbr2::HASHSIZE ? &rd.heads[s.segment * sbr2::HASHSIZE] : NULL;
            const uchar *key = NULL;
            for(int k = 0; k + 32 <= keys.length(); k += 32)
            {
                uchar id[8];
                sbr2::keyid(&keys[k], id);
                if(!memcmp(id, s.keyid, 8)) { key = &keys[k]; break; }
            }
            if(!key) unknown++;
            else if(!expected || memcmp(expected, s.hash, sbr2::HASHSIZE) || !s.check(key)) bad++;
            else { good++; if(s.scope == sbr2::signature::FINAL) final++; }
        }
        defformatstring(detail, "%d valid (%d final), %d unknown key, %d bad, %d trusted keys", good, final, unknown, bad, keys.length() / 32);
        if(bad) v.fail("signatures", detail);
        else if(!final) v.warn("signatures", detail);
        else v.pass("signatures", detail);
    }

    static void verifydata(verifier &v, const inputrun &run)
    {
        int files = 0, mismatched = 0;
        string first = "";
        loopvk(run.attempts) loopv(run.attempts[k]->files)
        {
            const mapdatafile &f = run.attempts[k]->files[i];
            files++;
            size_t len = 0;
            char *buf = loadfile(path(f.path, true), &len, false);
            uchar hash[32];
            bool same = false;
            if(buf)
            {
                blake3_hasher h;
                blake3_hasher_init(&h);
                blake3_hasher_update(&h, buf, len);
                blake3_hasher_finalize(&h, hash, sizeof(hash));
                same = len == f.size && !memcmp(hash, f.hash, sizeof(hash));
                delete[] buf;
            }
            if(!same && !mismatched++) copystring(first, f.path);
        }
        if(!files) { v.fail("data", "no file hashes (converted or older replay)"); return; }
        defformatstring(detail, "%d files%s%s", files, mismatched ? ", differs or missing: " : " match", first);
        v.check(!mismatched, "data", detail);
    }

    static void verifyrules(verifier &v, const inputrun &run, const replayinfo &info)
    {
        bool segmented = info.splices > 0;
        bool cleanstart = true;
        loopv(run.attempts) if(run.attempts[i]->snapshot.length() || run.attempts[i]->starttick) cleanstart = false;
        if(segmented) { defformatstring(d, "%d state loads", info.splices); v.warn("segmented", d); }
        else v.check(cleanstart, "start", cleanstart ? "map start" : "starts from a saved state");

        int settings = 0, free = 0, sourced = 0, unbacked = 0, slowmo = 0;
        long long pausedus = 0;
        int speedmin = 100, speedmax = 100, fixed = 0, timed = 0;
        loopvk(run.attempts)
        {
            const inputattempt &a = *run.attempts[k];
            loopv(a.settings) if(!strcmp(a.settings[i].name, "slowmosp") && a.settings[i].value) slowmo = 1;
            loopv(a.events)
            {
                const inputevent &e = a.events[i];
                if(e.type == INPUT_SETTING && e.source != INPUTSRC_MAP) settings++;
                if(e.type == INPUT_COMMAND && e.arg) free++;
                if(e.source == INPUTSRC_KEY || e.source == INPUTSRC_MOUSE)
                {
                    sourced++;
                    bool backed = false;
                    loopvj(a.raw) if(a.raw[j].tick == e.tick && a.raw[j].kind != rawevent::MOTION && a.raw[j].kind != rawevent::FOCUS) { backed = true; break; }
                    if(!backed) unbacked++;
                }
            }
            loopv(a.timing)
            {
                const segmenttiming &t = a.timing[i];
                timed++;
                pausedus += t.pausedus;
                speedmin = min(speedmin, t.speedmin);
                speedmax = max(speedmax, t.speedmax);
                fixed = max(fixed, t.fixedframe);
            }
        }
        defformatstring(d, "%d setting changes by the player", settings);
        v.check(!settings, "settings", d);
        formatstring(d, "%d commands outside the replay command list", free);
        v.check(!free, "commands", d);
        if(!timed) v.fail("timing", "no frame timing (converted or older replay)");
        else
        {
            formatstring(d, "%.3f s paused", pausedus / 1e6);
            if(segmented) v.warn("pause", d);
            else v.check(!pausedus, "pause", d);
            formatstring(d, "%d-%d%s", speedmin, speedmax, slowmo ? " (slowmosp)" : "");
            v.check(speedmax <= 100 && (speedmin >= 100 || slowmo), "gamespeed", d);
        }
        fixed = max(fixed, int(tellvalue(run, "pROV", "fixedframe", 0)));
        formatstring(d, "%d", fixed);
        v.check(!fixed, "fixedframe", d);

        if(!findtells(run, "pROV")) v.fail("provenance", "missing");
        else
        {
            static const char * const flags[] = { "harness", "dethash", "edit", "seedforced" };
            string bad = "";
            if(tellvalue(run, "pROV", "live", 0) != 1) concatstring(bad, " not-live");
            loopi(sizeof(flags)/sizeof(flags[0])) if(tellvalue(run, "pROV", flags[i], 0)) { concatstring(bad, " "); concatstring(bad, flags[i]); }
            v.check(!bad[0], "provenance", bad[0] ? bad : "live");
        }
        formatstring(d, "%d of %d key/mouse events without raw input", unbacked, sourced);
        v.check(!unbacked, "raw input", d);

        long long losses = tellvalue(run, "wIND", "focuslosses", 0), minimizes = tellvalue(run, "wIND", "minimizes", 0);
        formatstring(d, "%lld focus losses, %lld minimizes", losses, minimizes);
        if(losses || minimizes) v.warn("window", d);
        else v.pass("window", d);
    }

    static void replayverify(const char *file, const char *keyfile)
    {
        verifier v;
        stream *f = openrawfile(path(file, true), "rb");
        if(!f) { conoutf(CON_ERROR, "replayverify: could not open %s", file); intret(-1); return; }
        vector<uchar> buf;
        uchar chunk[65536];
        for(size_t n; (n = f->read(chunk, sizeof(chunk))) > 0;) buf.put(chunk, int(n));
        delete f;
        conoutf("replayverify: %s", file);
        sbr2::reader rd;
        inputrun run;
        replayinfo info;
        if(buf.length() < 4 || memcmp(buf.getbuf(), "SBR2", 4)) v.fail("format", "not an SBR2 file");
        else if(!rd.parse(buf.getbuf(), buf.length(), sbr2::KNOWNCHUNKS)) v.fail("container", rd.error);
        else if(!readsbr2(file, buf, &run, info)) v.fail("contents", "malformed");
        else
        {
            defformatstring(d, "%d segments", rd.heads.length() / sbr2::HASHSIZE);
            v.pass("container", d);
            verifysignatures(v, rd, keyfile);
            verifydata(v, run);
            verifyrules(v, run, info);
        }
        conoutf("replayverify: %d failed, %d warnings: %s", v.fails, v.warns, v.fails ? "\f3not eligible" : "\f0eligible");
        intret(v.fails);
    }
    ICOMMAND(replayverify, "ss", (char *file, char *keys), replayverify(file, keys));

    MOD(VARP, replayautosave, 0, 1, 1);
    MOD(VARP, replaymaxunfinished, 0, 20, 1000);

    static void formatrunfilename(char *dst, int ms)
    {
        nformatstring(dst, MAXSTRLEN, "%d-%02d-%03d", ms/60000, (ms/1000)%60, ms%1000);
    }

    static void safename(char *dst, const char *src)
    {
        int n = 0;
        for(const char *s = src; *s && n < 64; s++)
            dst[n++] = isalnum(uchar(*s)) || *s == '-' || *s == '_' ? *s : '_';
        dst[n] = '\0';
    }

    static const char *realpath(const char *file)
    {
        static string buf;
        copystring(buf, findfile(path(file, true), "wb"));
        return buf;
    }

    // autosaved unfinished replays are named ..._dnf, ..._dnf_seg (with state loads), plus _<n>
    static bool autounfinished(const char *name)
    {
        const char *dnf = strstr(name, "_dnf");
        if(!dnf) return false;
        dnf += 4;
        if(!strncmp(dnf, "_seg", 4)) dnf += 4;
        if(!*dnf) return true;
        if(*dnf++ != '_' || !*dnf) return false;
        for(; *dnf; dnf++) if(!isdigit(uchar(*dnf))) return false;
        return true;
    }

    // Keeps the newest replaymaxunfinished autosaved unfinished replays in replays/; finished, renamed
    // and kept (replays/kept/) ones are never deleted.
    static void rotatereplays()
    {
        vector<char *> files;
        listfiles("replays", "sbr", files);
        vector<char *> dnf;
        loopv(files) if(autounfinished(files[i])) dnf.add(files[i]);
        dnf.sort();
        for(int i = 0; i < dnf.length() - replaymaxunfinished; i++)
        {
            defformatstring(file, "replays/%s.sbr", dnf[i]);
            remove(realpath(file));
            statsreplaymoved(file, NULL);
        }
        files.deletearrays();
    }

    // the longest stretch of ticks without an input event
    int replaylongestidle(const inputrun &run)
    {
        int idle = 0;
        loopvk(run.attempts)
        {
            const inputattempt &a = *run.attempts[k];
            int last = a.starttick;
            loopv(a.events)
            {
                const inputevent &e = a.events[i];
                if(e.type == INPUT_SETTING) continue;
                idle = max(idle, e.tick - last);
                last = e.tick;
            }
            idle = max(idle, a.ticks - last);
        }
        return idle;
    }

    void fillreplayinfo(const inputrun &run, bool finished, int runtime, replayinfo &info)
    {
        info.started = 1;
        const inputattempt &first = *run.attempts[0];
        info.version = sbr2::VERSION;
        info.simversion = SIMVERSION;
        info.date = uint(time(NULL));
        info.live = true;
        copystring(info.client, "speedbraten");
        copystring(info.player, player1->name);
        info.mode = first.mode;
        info.flagrun = first.flagrun;
        info.race = first.race;
        info.finished = finished ? 1 : 0;
        info.runtime = finished ? runtime : 0;
        info.ticks = info.splices = 0;
        loopv(run.attempts) { info.ticks += run.attempts[i]->ticks - run.attempts[i]->starttick; info.splices += run.attempts[i]->splices.length(); }
        copystring(info.map, first.map);
    }

    // started: the run counts for the statistics (a flagrun or race life whose run began)
    void saverunreplay(const inputrun &run, bool finished, int runtime, bool started)
    {
        if(run.empty()) return;
        const inputattempt &first = *run.attempts[0];
        replayinfo info;
        fillreplayinfo(run, finished, runtime, info);
        inputrun traced;
        if(finished && replayautosave)
        {
            run.copyto(traced);
            ghostattach(traced);
        }
        info.started = started || finished ? 1 : 0;
        if(!replayautosave)
        {
            if(started) statsrecord(run, info, NULL);
            return;
        }

        time_t t = time(NULL);
        tm *lt = localtime(&t);
        string map, result, file;
        safename(map, first.map);
        if(finished) formatrunfilename(result, runtime);
        else copystring(result, "dnf");
        if(info.splices) concatstring(result, "_seg");
        formatstring(file, "replays/%04d-%02d-%02d_%02d-%02d-%02d_%s_%s.sbr", lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                     lt->tm_hour, lt->tm_min, lt->tm_sec, map, result);
        for(int n = 2; fileexists(realpath(file), "r") && n < 100; n++)
            formatstring(file, "replays/%04d-%02d-%02d_%02d-%02d-%02d_%s_%s_%d.sbr", lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                         lt->tm_hour, lt->tm_min, lt->tm_sec, map, result, n);
        uchar content[sbr2::HASHSIZE];
        bool written = writereplay(file, finished ? traced : run, info, content);
        if(started) statsrecord(run, info, written ? file : NULL, written ? content : NULL);
        if(!written) { conoutf(CON_ERROR, "could not write %s", file); return; }
        if(finished) conoutf("\f2saved replay %s", file);
        else rotatereplays();
    }

    // Replay browser: replayscan caches the headers of all replay files, newest first.
    struct replayentry
    {
        string file;
        bool kept;
        int size;
        replayinfo info;
    };
    static vector<replayentry> replaylist;

    MOD(SVARP, replayfilter, "");
    MOD(VARP, replaysort, 0, 0, 1);

    static bool newerreplay(const replayentry &a, const replayentry &b) { return a.info.date > b.info.date; }

    // finished runs fastest first, then unfinished ones newest first
    static bool fasterreplay(const replayentry &a, const replayentry &b)
    {
        if(a.info.finished != b.info.finished) return a.info.finished > b.info.finished;
        if(a.info.finished && a.info.runtime != b.info.runtime) return a.info.runtime < b.info.runtime;
        return a.info.date > b.info.date;
    }

    static bool containsnocase(const char *s, const char *sub)
    {
        if(!sub[0]) return true;
        for(; *s; s++)
        {
            int i = 0;
            while(sub[i] && s[i] && tolower(uchar(s[i])) == tolower(uchar(sub[i]))) i++;
            if(!sub[i]) return true;
        }
        return false;
    }

    static void scanreplaydir(const char *dir, bool kept)
    {
        vector<char *> files;
        listfiles(dir, "sbr", files);
        loopv(files)
        {
            replayentry e;
            formatstring(e.file, "%s/%s.sbr", dir, files[i]);
            if(!readreplay(e.file, NULL, e.info) || !containsnocase(e.info.map, replayfilter)) continue;
            e.kept = kept;
            stream *f = openrawfile(path(e.file, true), "rb");
            e.size = f ? int(f->size()) : 0;
            delete f;
            replaylist.add(e);
        }
        files.deletearrays();
    }

    ICOMMAND(replayscan, "", (),
    {
        replaylist.setsize(0);
        scanreplaydir("replays", false);
        scanreplaydir("replays/kept", true);
        replaylist.sort(replaysort ? fasterreplay : newerreplay);
        intret(replaylist.length());
    });

    ICOMMAND(replayfield, "is", (int *i, char *field),
    {
        if(!replaylist.inrange(*i)) { result(""); return; }
        const replayentry &e = replaylist[*i];
        const replayinfo &r = e.info;
        if(!strcmp(field, "file")) result(e.file);
        else if(!strcmp(field, "name"))
        {
            const char *base = strrchr(e.file, '/');
            string name;
            copystring(name, base ? base+1 : e.file);
            int len = strlen(name);
            if(len > 4) name[len-4] = '\0';
            result(name);
        }
        else if(!strcmp(field, "date") || !strcmp(field, "time"))
        {
            time_t t = r.date;
            tm *lt = localtime(&t);
            string buf;
            if(field[0] == 'd') formatstring(buf, "%04d-%02d-%02d", lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday);
            else formatstring(buf, "%02d:%02d", lt->tm_hour, lt->tm_min);
            result(buf);
        }
        else if(!strcmp(field, "map")) result(r.map);
        else if(!strcmp(field, "maps")) intret(r.maps);
        else if(!strcmp(field, "mode")) result(server::modename(r.mode, "?"));
        else if(!strcmp(field, "flagrun")) intret(r.flagrun);
        else if(!strcmp(field, "race")) intret(r.race);
        else if(!strcmp(field, "splices")) intret(r.splices);
        else if(!strcmp(field, "player")) result(r.player);
        else if(!strcmp(field, "finished")) intret(r.finished);
        else if(!strcmp(field, "runtime"))
        {
            string buf;
            if(r.finished) formatstring(buf, "%d:%02d.%03d", r.runtime/60000, (r.runtime/1000)%60, r.runtime%1000);
            else copystring(buf, "DNF");
            result(buf);
        }
        else if(!strcmp(field, "length"))
        {
            int ms = r.ticks*SIMTICK;
            defformatstring(buf, "%d:%02d", ms/60000, (ms/1000)%60);
            result(buf);
        }
        else if(!strcmp(field, "size"))
        {
            defformatstring(buf, "%.1f KB", e.size/1024.0f);
            result(buf);
        }
        else if(!strcmp(field, "kept")) intret(e.kept ? 1 : 0);
        else if(!strcmp(field, "outdated")) intret(r.simversion < REPLAYSIMVERSION ? 1 : 0);
        else result("");
    });

    ICOMMAND(replayplay, "i", (int *i),
    {
        if(!replaylist.inrange(*i) || multiplayer(false)) return;
        inputrun run;
        replayinfo info;
        if(!readreplay(replaylist[*i].file, &run, info)) { conoutf(CON_ERROR, "could not read %s", replaylist[*i].file); return; }
        playinputrun(run, replaylist[*i].file);
    });

    ICOMMAND(replaydelete, "i", (int *i),
    {
        if(!replaylist.inrange(*i)) return;
        remove(realpath(replaylist[*i].file));
        statsreplaymoved(replaylist[*i].file, NULL);
        replaylist.remove(*i);
    });

    static bool movereplay(replayentry &e, const char *dir, const char *name)
    {
        string dst;
        formatstring(dst, "%s/%s.sbr", dir, name);
        string from;
        copystring(from, realpath(e.file));
        const char *to = realpath(dst);
        if(fileexists(to, "r") || rename(from, to)) return false;
        statsreplaymoved(e.file, dst);
        copystring(e.file, dst);
        return true;
    }

    ICOMMAND(replaykeep, "i", (int *i),
    {
        if(!replaylist.inrange(*i)) return;
        replayentry &e = replaylist[*i];
        const char *base = strrchr(e.file, '/');
        string name;
        copystring(name, base ? base+1 : e.file);
        int len = strlen(name);
        if(len > 4) name[len-4] = '\0';
        if(movereplay(e, e.kept ? "replays" : "replays/kept", name)) e.kept = !e.kept;
        else conoutf(CON_ERROR, "could not move %s", e.file);
    });

    ICOMMAND(replayrename, "is", (int *i, char *newname),
    {
        if(!replaylist.inrange(*i) || !newname[0]) return;
        replayentry &e = replaylist[*i];
        string name;
        safename(name, newname);
        if(!movereplay(e, e.kept ? "replays/kept" : "replays", name)) conoutf(CON_ERROR, "could not rename %s", e.file);
    });
}

// Replay cleanup rules, run on demand: cleanupcheck lists the replays in replays/ (never
// replays/kept/) that a rule matches, cleanupdelete removes the listed ones. A rule is
// "<kind> <result> <condition> <n>": kind any|sp|flagrun|race, result any|unfinished|finished,
// condition shorter (n s), idle (an idle stretch longer than n s), notstarted, outdated (made
// with an older simulation, so it will likely desync), older (n days) or keepbest (matches all
// but the n fastest finished runs per map and mode).
namespace game
{
    MOD(SVARP, replaycleanuprules, "");

    struct cleanuprule
    {
        string kind, result, cond;
        int n;
    };

    static void parserules(vector<cleanuprule> &rules)
    {
        vector<char *> parts;
        explodelist(replaycleanuprules, parts);
        for(int i = 0; i + 3 < parts.length(); i += 4)
        {
            cleanuprule &r = rules.add();
            copystring(r.kind, parts[i]);
            copystring(r.result, parts[i+1]);
            copystring(r.cond, parts[i+2]);
            r.n = parseint(parts[i+3]);
        }
        parts.deletearrays();
    }

    static void saverules(const vector<cleanuprule> &rules)
    {
        vector<char> buf;
        loopv(rules)
        {
            defformatstring(s, "%s%s %s %s %d", i ? " " : "", rules[i].kind, rules[i].result, rules[i].cond, rules[i].n);
            buf.put(s, strlen(s));
        }
        buf.add('\0');
        setsvar("replaycleanuprules", buf.getbuf());
    }

    static const char *replaykind(const replayinfo &info) { return info.race ? "race" : info.flagrun ? "flagrun" : "sp"; }

    struct cleanupfile
    {
        string file;
        replayinfo info;
        int idle, size, rule;
    };
    static vector<cleanupfile> cleanuplist;

    static bool rulefilter(const cleanuprule &r, const replayinfo &info)
    {
        if(strcmp(r.kind, "any") && strcmp(r.kind, replaykind(info))) return false;
        if(!strcmp(r.result, "unfinished") && info.finished) return false;
        if(!strcmp(r.result, "finished") && !info.finished) return false;
        return true;
    }

    static bool fastercleanup(const cleanupfile *a, const cleanupfile *b)
    {
        return a->info.runtime != b->info.runtime ? a->info.runtime < b->info.runtime : a->info.date > b->info.date;
    }

    static void checkcleanup()
    {
        cleanuplist.setsize(0);
        vector<cleanuprule> rules;
        parserules(rules);
        if(rules.empty()) return;
        vector<cleanupfile> all;
        vector<char *> files;
        listfiles("replays", "sbr", files);
        loopv(files)
        {
            cleanupfile c;
            formatstring(c.file, "replays/%s.sbr", files[i]);
            inputrun run;
            if(!readreplay(c.file, &run, c.info)) continue;
            c.idle = replaylongestidle(run);
            stream *f = openrawfile(path(c.file, true), "rb");
            c.size = f ? int(f->size()) : 0;
            delete f;
            c.rule = -1;
            all.add(c);
        }
        files.deletearrays();
        time_t now = time(NULL);
        loopvj(rules)
        {
            const cleanuprule &r = rules[j];
            if(!strcmp(r.cond, "keepbest"))
            {
                // per map, mode, kind and segmented or not: the fastest n finished runs stay
                vector<cleanupfile *> group;
                loopv(all)
                {
                    cleanupfile &c = all[i];
                    if(!c.info.finished || !rulefilter(r, c.info)) continue;
                    group.setsize(0);
                    loopvk(all)
                    {
                        cleanupfile &o = all[k];
                        if(o.info.finished && o.info.mode == c.info.mode && !strcmp(replaykind(o.info), replaykind(c.info)) &&
                           (o.info.splices > 0) == (c.info.splices > 0) && !strcmp(o.info.map, c.info.map)) group.add(&o);
                    }
                    group.sort(fastercleanup);
                    if(group.find(&c) >= max(r.n, 0) && c.rule < 0) c.rule = j;
                }
                continue;
            }
            loopv(all)
            {
                cleanupfile &c = all[i];
                if(c.rule >= 0 || !rulefilter(r, c.info)) continue;
                bool match = false;
                if(!strcmp(r.cond, "shorter")) match = c.info.ticks*SIMTICK < r.n*1000;
                else if(!strcmp(r.cond, "idle")) match = c.idle*SIMTICK > r.n*1000;
                else if(!strcmp(r.cond, "notstarted")) match = c.info.started == 0;
                else if(!strcmp(r.cond, "outdated")) match = c.info.simversion < REPLAYSIMVERSION;
                else if(!strcmp(r.cond, "older")) match = double(now) - double(c.info.date) > r.n*86400.0;
                if(match) c.rule = j;
            }
        }
        loopv(all) if(all[i].rule >= 0) cleanuplist.add(all[i]);
    }

    ICOMMAND(cleanupadd, "sssi", (char *kind, char *result, char *cond, int *n),
    {
        vector<cleanuprule> rules;
        parserules(rules);
        cleanuprule &r = rules.add();
        copystring(r.kind, kind);
        copystring(r.result, result);
        copystring(r.cond, cond);
        r.n = *n;
        saverules(rules);
    });
    ICOMMAND(cleanupremove, "i", (int *i),
    {
        vector<cleanuprule> rules;
        parserules(rules);
        if(rules.inrange(*i)) rules.remove(*i);
        saverules(rules);
    });
    ICOMMAND(cleanuprulecount, "", (), { vector<cleanuprule> rules; parserules(rules); intret(rules.length()); });
    static void ruletext(int i, char *res)
    {
        vector<cleanuprule> rules;
        parserules(rules);
        res[0] = '\0';
        if(!rules.inrange(i)) return;
        const cleanuprule &r = rules[i];
        bool anyresult = !strcmp(r.result, "any");
        string what, cond;
        formatstring(what, "%s%s%s", anyresult ? "" : r.result, anyresult ? "" : " ", strcmp(r.kind, "any") ? r.kind : "all");
        if(!strcmp(r.cond, "shorter")) formatstring(cond, "shorter than %d s", r.n);
        else if(!strcmp(r.cond, "idle")) formatstring(cond, "with no input for over %d s", r.n);
        else if(!strcmp(r.cond, "notstarted")) copystring(cond, "where the run never started");
        else if(!strcmp(r.cond, "outdated")) copystring(cond, "made with an older build (will likely desync)");
        else if(!strcmp(r.cond, "older")) formatstring(cond, "older than %d days", r.n);
        else if(!strcmp(r.cond, "keepbest")) formatstring(cond, "except the %d fastest per map and mode", r.n);
        else copystring(cond, r.cond);
        nformatstring(res, MAXSTRLEN, "%s runs %s", what, cond);
    }
    ICOMMAND(cleanupruletext, "i", (int *i), { string res; ruletext(*i, res); result(res); });

    // cleanupcheck: lists the matching replays and returns their number
    ICOMMAND(cleanupcheck, "", (), { checkcleanup(); intret(cleanuplist.length()); });
    ICOMMAND(cleanupsize, "", (),
    {
        int total = 0;
        loopv(cleanuplist) total += cleanuplist[i].size;
        defformatstring(res, "%.1f MB", total/(1024.0f*1024.0f));
        result(res);
    });
    ICOMMAND(cleanupfield, "is", (int *i, char *field),
    {
        if(!cleanuplist.inrange(*i)) { result(""); return; }
        const cleanupfile &c = cleanuplist[*i];
        if(!strcmp(field, "file")) result(c.file);
        else if(!strcmp(field, "rule")) intret(c.rule);
        else result("");
    });
    // cleanupdelete: removes the replays the last cleanupcheck listed, returns how many
    ICOMMAND(cleanupdelete, "", (),
    {
        int n = 0;
        loopv(cleanuplist)
        {
            if(remove(realpath(cleanuplist[i].file))) continue;
            statsreplaymoved(cleanuplist[i].file, NULL);
            n++;
        }
        conoutf("cleanup: deleted %d replays", n);
        cleanuplist.setsize(0);
        intret(n);
    });
}
