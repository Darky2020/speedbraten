// Replay files: a run's recorded input plus metadata in SBR2 form (the container is in
// replayio.h, the run layout below), in replays/ (and replays/kept/ for runs protected from
// the rolling cleanup of unfinished attempts).

#include "game.h"
#include "sbcompat.h"
#include "inputs.h"
#include "replayio.h"
#include "blake3.h"
#include "witnessproto.h"
#include "hubproto.h"

namespace game
{
    // SIMVERSION: the simulation's version, stored in every replay. REPLAYSIMVERSION: oldest
    // version the current simulation still plays back; bump both when the simulation changes.
    enum { SIMVERSION = 11, REPLAYSIMVERSION = 2, REPLAYHASHSTEP = 50 };
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

    // SBR2 files, container version 2 (the container is in replayio.h). Everything before the
    // first segment is known when the run starts, so a file can be streamed while it is played.
    //   META   key-value pairs, keys sorted: client, date, flagrun, map, mode, player, race
    //   per attempt:
    //     DATA var sim version, var files, per file: u8 role, str path, var size, BLAKE3
    //     ATTM var index, str map, u32 map crc, u8 has cfg crc, u32 cfg crc, u32 seed, str team,
    //          svar mode, var flagrun, var race, var start tick, var hash groups, var flags (1: every
    //          spawn in a flagrun or race reseeds the RNG with lifeseed(); 2: the attempt continues
    //          the previous run of the same life from where it ended), var files, var
    //          index per file into the attempt's DATA, var settings, (str name, svar value) per
    //          setting, var saved state length, saved state
    //     per segment of segmentticks() ticks [lo, hi), by absolute tick:
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
    //       tIME (optional) var wall us, var paused us, var main loop passes ("frames"), var
    //            passes without a tick, var passes with 2+ ticks, 8 x var pass-time histogram,
    //            var min gamespeed, var max gamespeed, var max fixedframe, then (may be absent)
    //            var passes that drew the screen
    //       bIND (ahead of the rAWI that first uses them) var binds, str per bind: bind actions
    //            appended to the list rAWI's bind numbers refer to
    //       SEGE
    //   gHST   (optional, per attempt) var attempt index, var samples; columns: var run tick
    //          delta; svar x, y, z in 1/GHOSTPOSSCALE units and svar yaw, pitch in
    //          1/GHOSTANGLESCALE degrees (each minus the previous sample's); var flags
    //   RSLT   u8 finished, u8 started (2 unknown), var ticks, var runtime ms, var splices, var
    //          attempts, var ticks per attempt
    //   run metadata (see tells.cpp), key-value: cLNT, sESS, hIST, pROV, fEAT, SETS, wIND, rSTA,
    //          dEVS
    //   END
    // Version 1 files are still read: META also holds maps, one DATA holds every attempt's files,
    // the run metadata except wIND and rSTA and one bIND come before the first ATTM, ATTM holds
    // the attempt's ticks, and gHST (without the index) follows the attempt's segments.
    // longer segments compress better; runs in the other modes are short and get a witness
    // signature every second
    static int segmentticks(const inputattempt &a) { return m_check(a.mode, M_DMSP | M_CLASSICSP) ? 5*REPLAYSEGMENTTICKS : REPLAYSEGMENTTICKS; }

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
        "cleargui", "clearmapguis", "team"
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
        loopi(n) w.u32(hashes[i]->combined ? hashes[i]->groups[0] : combinedhash(hashes[i]->groups, groups));
        if(n) loopi(groups) w.u32(hashes[n-1]->groups[i]);
    }

    static void writedata(sbr2::writer &out, int simversion, const vector<const mapdatafile *> &files)
    {
        vector<uchar> p;
        sbr2::byteswriter w(p);
        w.var(simversion);
        w.var(files.length());
        loopv(files)
        {
            w.u8(files[i]->role);
            w.str(files[i]->path);
            w.var(files[i]->size);
            w.bytes(files[i]->hash, sizeof(files[i]->hash));
        }
        out.add("DATA", p);
    }

    static void writetrace(sbr2::writer &out, int index, const inputattempt &a);

    // A replay file written front to back, one segment at a time, so it can be sent while the
    // run is played: the bytes after n segments are the first bytes of the finished file.
    // Everything before the segments is known when the run starts, and a segment is written
    // only once it is complete. An attempt's data can come from a live recording that holds
    // more than the run: skip leaves out everything at or before that tick, as cutattempt does.
    struct sbr2stream
    {
        sbr2::writer out;
        vector<const char *> binds;
        const inputattempt *data;
        int skip, seg, ev, hs, sp, tm, rw;
        int fixedsegment; // ticks per segment whatever the mode; 0: segmentticks()

        sbr2stream() : data(NULL), skip(-1), seg(0), ev(0), hs(0), sp(0), tm(0), rw(0), fixedsegment(0) {}

        int ticksper(const inputattempt &a) const { return fixedsegment ? fixedsegment : segmentticks(a); }

        void meta(const inputrun &run, const replayinfo &info)
        {
            vector<uchar> p;
            sbr2::byteswriter w(p);
            w.var(7);
            kvkey(w, "client", sbr2::VALUE_STR); w.str(info.client);
            kvkey(w, "date", sbr2::VALUE_VAR); w.var(info.date);
            kvkey(w, "flagrun", sbr2::VALUE_VAR); w.var(info.flagrun);
            kvkey(w, "map", sbr2::VALUE_STR); utf8str(w, run.attempts.length() ? run.attempts[0]->map : "");
            kvkey(w, "mode", sbr2::VALUE_SVAR); w.svar(info.mode);
            kvkey(w, "player", sbr2::VALUE_STR); utf8str(w, info.player);
            kvkey(w, "race", sbr2::VALUE_VAR); w.var(info.race);
            out.add("META", p);
        }

        // header: the attempt as stored (map, settings, saved state, start tick); d: where its
        // events, hashes and timing are read from
        void beginattempt(int index, const inputattempt &header, const inputattempt &d, int simversion, int skiptick = -1)
        {
            vector<const mapdatafile *> files;
            loopv(header.files) if(datafileindex(files, header.files[i]) < 0) files.add(&header.files[i]);
            writedata(out, simversion, files);

            vector<uchar> p;
            sbr2::byteswriter w(p);
            w.var(index);
            w.str(header.map);
            w.u32(header.mapcrc);
            w.u8(header.hascfgcrc ? 1 : 0);
            w.u32(header.cfgcrc);
            w.u32(header.seed);
            w.str(header.team);
            w.svar(header.mode);
            w.var(header.flagrun);
            w.var(header.race);
            w.var(header.starttick);
            w.var(header.hashgroups);
            w.var((header.lifeseeds ? 1 : 0) | (header.chained ? 2 : 0));
            w.var(header.files.length());
            loopv(header.files) w.var(datafileindex(files, header.files[i]));
            w.var(header.settings.length());
            loopv(header.settings) { w.str(header.settings[i].name); w.svar(header.settings[i].value); }
            w.var(header.snapshot.length());
            w.bytes(header.snapshot.getbuf(), header.snapshot.length());
            out.add("ATTM", p, true);

            data = &d;
            skip = skiptick;
            ev = hs = sp = rw = tm = 0;
            while(ev < d.events.length() && d.events[ev].tick <= skip) ev++;
            while(hs < d.hashes.length() && d.hashes[hs].tick <= skip) hs++;
            while(sp < d.splices.length() && d.splices[sp].tick <= skip) sp++;
            while(rw < d.raw.length() && d.raw[rw].tick <= skip) rw++;
            if(skip >= 0) while(tm < d.timing.length() && d.timing[tm].segment < skip / REPLAYSEGMENTTICKS) tm++;
            int first = header.starttick;
            if(ev < d.events.length()) first = min(first, d.events[ev].tick);
            if(hs < d.hashes.length()) first = min(first, d.hashes[hs].tick);
            if(sp < d.splices.length()) first = min(first, d.splices[sp].tick);
            if(rw < d.raw.length()) first = min(first, d.raw[rw].tick);
            seg = max(first, 0) / ticksper(d);
        }

        int segmentend() const { return (seg + 1) * ticksper(*data); }

        // the last segment holds the attempt's last tick, event, hash or raw event
        int lastsegment() const
        {
            const inputattempt &a = *data;
            int last = a.ticks;
            if(a.events.length()) last = max(last, a.events[a.events.length()-1].tick);
            if(a.hashes.length()) last = max(last, a.hashes[a.hashes.length()-1].tick);
            if(a.splices.length()) last = max(last, a.splices[a.splices.length()-1].tick);
            if(a.raw.length()) last = max(last, a.raw[a.raw.length()-1].tick);
            return last / ticksper(a);
        }

        // The next segment. Besides every REPLAYHASHSTEP-th hash it keeps the hash of endtick,
        // the attempt's last tick (-1 while the attempt goes on).
        void writesegment(int endtick)
        {
            const inputattempt &a = *data;
            int segticks = ticksper(a), lo = seg*segticks, hi = lo + segticks;
            vector<uchar> p;
            sbr2::byteswriter w(p);
            int n = 0;
            while(ev + n < a.events.length() && a.events[ev + n].tick < hi) n++;
            w.var(lo);
            w.var(hi - lo);
            writeevents(w, a.events.getbuf() + ev, n, lo);
            ev += n;
            out.add("INPS", p, true);

            n = 0;
            while(rw + n < a.raw.length() && a.raw[rw + n].tick < hi) n++;
            if(n)
            {
                int known = binds.length();
                loopi(n)
                {
                    const char *bind = a.raw[rw + i].bind;
                    if(bind && binds.find(bind) < 0) binds.add(bind);
                }
                if(binds.length() > known)
                {
                    p.setsize(0);
                    w.var(binds.length() - known);
                    for(int i = known; i < binds.length(); i++) w.str(binds[i]);
                    out.add("bIND", p, true);
                }
                p.setsize(0);
                writeraw(w, &a.raw[rw], n, lo, binds);
                rw += n;
                out.add("rAWI", p, true);
            }

            vector<const tickhash *> hashes;
            for(; hs < a.hashes.length() && a.hashes[hs].tick < hi; hs++)
                if(a.hashes[hs].tick % REPLAYHASHSTEP == 0 || a.hashes[hs].tick == endtick) hashes.add(&a.hashes[hs]);
            p.setsize(0);
            writehashes(w, hashes.getbuf(), hashes.length(), lo, a.hashgroups);
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

            // timing is kept per second; this segment's seconds are merged
            segmenttiming t(lo / REPLAYSEGMENTTICKS);
            int seconds = 0;
            while(tm < a.timing.length() && a.timing[tm].segment < lo / REPLAYSEGMENTTICKS) tm++;
            for(; tm < a.timing.length() && a.timing[tm].segment < hi / REPLAYSEGMENTTICKS; tm++)
            {
                const segmenttiming &s = a.timing[tm];
                t.wallus += s.wallus;
                t.pausedus += s.pausedus;
                t.frames0 += s.frames0;
                t.frames2 += s.frames2;
                t.drawn = t.drawn < 0 || s.drawn < 0 ? -1 : t.drawn + s.drawn;
                loopi(segmenttiming::BUCKETS) t.hist[i] += s.hist[i];
                t.speedmin = seconds++ ? min(t.speedmin, s.speedmin) : s.speedmin;
                t.speedmax = max(t.speedmax, s.speedmax);
                t.fixedframe = max(t.fixedframe, s.fixedframe);
                t.frames += s.frames;
            }
            if(seconds)
            {
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
                if(t.drawn >= 0) w.var(t.drawn);
                out.add("tIME", p, true);
            }
            out.endsegment(hi);
            seg++;
        }

        // tells: the run metadata chunks, if already made (made from run and info otherwise)
        void trailer(const inputrun &run, const replayinfo &info, const vector<runchunk *> *given = NULL)
        {
            loopv(run.attempts) writetrace(out, i, *run.attempts[i]);
            vector<uchar> p;
            sbr2::byteswriter w(p);
            w.u8(info.finished ? 1 : 0);
            w.u8(info.started < 0 ? 2 : info.started ? 1 : 0);
            w.var(info.ticks);
            w.var(info.runtime);
            w.var(info.splices);
            w.var(run.attempts.length());
            loopv(run.attempts) w.var(run.attempts[i]->ticks);
            out.add("RSLT", p);

            vector<runchunk *> made;
            if(!given && info.live) tellsencode(run, info, made);
            const vector<runchunk *> &tells = given ? *given : info.live ? made : run.tells;
            loopv(tells) out.add(tells[i]->type, tells[i]->data);
            made.deletecontents();
            out.finish();
        }
    };

    static void writetrace(sbr2::writer &out, int index, const inputattempt &a)
    {
        if(a.trace.empty()) return;
        vector<uchar> p;
        sbr2::byteswriter w(p);
        w.var(index);
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

    // The whole file, or with maxsegments = n only its first n segments (no trailer, no END).
    static void buildsbr2(sbr2stream &s, const inputrun &run, const replayinfo &info, int maxsegments = -1, const vector<runchunk *> *tells = NULL)
    {
        s.meta(run, info);
        loopv(run.attempts)
        {
            const inputattempt &a = *run.attempts[i];
            s.beginattempt(i, a, a, info.simversion);
            for(int last = s.lastsegment(); s.seg <= last; )
            {
                if(maxsegments >= 0 && s.out.segments >= maxsegments) return;
                s.writesegment(a.ticks);
            }
        }
        if(maxsegments < 0) s.trailer(run, info, tells);
    }

    static bool writesbr2(const char *file, const inputrun &run, const replayinfo &info, uchar *content = NULL)
    {
        sbr2stream s;
        buildsbr2(s, run, info);
        const sbr2::writer &out = s.out;
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

    static bool readattempt(sbr2::bytesreader &r, inputattempt &a, const vector<mapdatafile> &files, int version)
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
        if(version < 2) a.ticks = int(r.var());
        a.hashgroups = int(r.var());
        unsigned long long flags = version >= 2 ? r.var() : 0;
        a.lifeseeds = (flags & 1) != 0;
        a.chained = (flags & 2) != 0;
        if(!r.ok || hascfgcrc > 1 || a.hashgroups < 1 || a.hashgroups > DETHASH_GROUPS || flags > 3) return false;
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
        t.drawn = r.left() ? int(r.var()) : -1;
        return r.ok;
    }

    static bool readtrace(sbr2::bytesreader &r, inputattempt &a)
    {
        if(a.trace.length()) return false;
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

    static bool readdata(sbr2::bytesreader &r, replayinfo &info, vector<mapdatafile> &files)
    {
        files.setsize(0);
        info.simversion = int(r.var());
        int n = int(r.var());
        loopj(n)
        {
            if(!r.ok) return false;
            mapdatafile &f = files.add();
            f.role = r.u8();
            r.str(f.path, sizeof(f.path));
            // replays made on Windows before paths were stored with '/'
            for(char *s = f.path; *s; s++) if(*s == '\\') *s = '/';
            f.size = uint(r.var());
            r.bytes(f.hash, sizeof(f.hash));
        }
        return r.ok;
    }

    static bool readbinds(sbr2::bytesreader &r, vector<const char *> &binds)
    {
        int n = int(r.var());
        loopj(n)
        {
            unsigned long long len = r.var();
            if(!r.ok || len > (unsigned long long)r.left()) return false;
            binds.add(internbind((const char *)r.p, int(len)));
            r.p += len;
        }
        return r.ok;
    }

    // Version 1: META, DATA (every map's files), run tells, bIND, then per attempt ATTM (with
    // its tick count), its segments and gHST; RSLT and the late tells. Version 2: META, then per
    // attempt DATA (that map's files), ATTM and segments, with bIND ahead of the segment that
    // first uses a bind; then gHST per attempt, RSLT (with the attempt tick counts) and every
    // tell. With run == NULL the attempts are skipped, but the whole file is still checked.
    // known != NULL reads the front of a version 2 file that is still being written: chunks end
    // with a segment or an attempt header, and there is no trailer. known is set to the tick up
    // to which the last attempt can be played: the start of its last closed segment, as a
    // segment spans a fixed number of ticks and the attempt may end anywhere in its last one
    // (0 before it has closed one).
    static bool readsbr2chunks(const char *file, const vector<sbr2::chunk *> &chunks, int version, inputrun *run, replayinfo &info, int *known = NULL)
    {
        info = replayinfo();
        info.version = version;
        bool v2 = version >= 2;
        if(run) run->clear();
        vector<mapdatafile> files;
        bool meta = false, data = false, result = false, dataforattempt = false;
        int attempts = 0, lo = 0, hi = 0, nexttick = 0, inpssegment = -1, lastlo = 0;
        inputattempt *a = NULL;
        vector<const char *> binds;
        loopv(chunks)
        {
            const sbr2::chunk &c = *chunks[i];
            sbr2::bytesreader r(c.data.getbuf(), c.data.length());
            bool ok = true, parsed = true;
            if(!memcmp(c.type, "META", 4)) { ok = !meta && i == 0 && readmeta(r, info); meta = true; }
            else if(!memcmp(c.type, "DATA", 4))
            {
                ok = meta && !result && (v2 ? !dataforattempt : !data) && readdata(r, info, files);
                data = dataforattempt = true;
            }
            else if(!memcmp(c.type, "ATTM", 4))
            {
                ok = data && !result && (!v2 || dataforattempt) && int(r.var()) == attempts;
                dataforattempt = false;
                attempts++;
                nexttick = lastlo = 0;
                inpssegment = -1;
                a = run ? &run->add() : NULL;
                if(a && ok) ok = readattempt(r, *a, files, version);
                else parsed = false;
            }
            else if(!memcmp(c.type, "bIND", 4))
            {
                ok = data && !result && (v2 || (!attempts && binds.empty())) && readbinds(r, binds);
            }
            else if(!memcmp(c.type, "gHST", 4))
            {
                ok = attempts > 0 && !result;
                if(v2)
                {
                    int index = int(r.var());
                    ok = ok && r.ok && index >= 0 && index < attempts;
                    if(ok && run) ok = readtrace(r, *run->attempts[index]);
                    else parsed = false;
                }
                else if(!a) parsed = false;
                else if(ok) ok = readtrace(r, *a);
            }
            else if(!memcmp(c.type, "INPS", 4) || !memcmp(c.type, "HASH", 4) || !memcmp(c.type, "SPLC", 4) || !memcmp(c.type, "tIME", 4) || !memcmp(c.type, "rAWI", 4))
            {
                ok = attempts > 0 && !result;
                if(!a) parsed = false;
                else if(!ok) {}
                else if(!memcmp(c.type, "INPS", 4))
                {
                    ok = inpssegment != c.segment && readevents(r, *a, lo, hi) && lo >= nexttick;
                    nexttick = hi;
                    lastlo = lo;
                    inpssegment = c.segment;
                }
                else if(!memcmp(c.type, "tIME", 4)) ok = inpssegment == c.segment && readtiming(r, *a, lo / REPLAYSEGMENTTICKS);
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
                if(v2 && ok)
                {
                    info.maps = int(r.var());
                    ok = r.ok && info.maps == attempts;
                    loopj(attempts)
                    {
                        int ticks = int(r.var());
                        if(run && ok) run->attempts[j]->ticks = ticks;
                    }
                }
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
        if(known)
        {
            *known = lastlo;
            return v2 && meta && !result;
        }
        if(!meta || !data || !result || attempts != info.maps)
        {
            conoutf(CON_ERROR, "replay %s: incomplete", file);
            if(run) run->clear();
            return false;
        }
        return true;
    }

    static bool readsbr2(const char *file, const vector<uchar> &buf, inputrun *run, replayinfo &info)
    {
        sbr2::reader rd;
        if(!rd.parse(buf.getbuf(), buf.length(), sbr2::KNOWNCHUNKS)) { conoutf(CON_ERROR, "replay %s: %s", file, rd.error); return false; }
        return readsbr2chunks(file, rd.chunks, rd.version, run, info);
    }

    static bool attemptheaderchunk(const sbr2::chunk &c)
    {
        return !memcmp(c.type, "DATA", 4) || !memcmp(c.type, "ATTM", 4) || !memcmp(c.type, "bIND", 4);
    }

    // The front of a file that is still being written, read as far as it is complete: every
    // closed segment, plus the header of an attempt whose first segment hasn't closed yet.
    static bool readsbr2prefix(const char *file, const vector<uchar> &buf, inputrun &run, replayinfo &info, int &known)
    {
        sbr2::streamreader rd;
        if(!rd.feed(buf.getbuf(), buf.length(), sbr2::KNOWNCHUNKS)) { conoutf(CON_ERROR, "replay %s: %s", file, rd.error); return false; }
        int end = 0;
        loopv(rd.chunks) if(!memcmp(rd.chunks[i]->type, "SEGE", 4)) end = i+1;
        if(!end) while(end < rd.chunks.length() && (!memcmp(rd.chunks[end]->type, "META", 4) || attemptheaderchunk(*rd.chunks[end]))) end++;
        else while(end < rd.chunks.length() && attemptheaderchunk(*rd.chunks[end])) end++;
        vector<sbr2::chunk *> front;
        loopi(end) front.add(rd.chunks[i]);
        return readsbr2chunks(file, front, rd.version, &run, info, &known);
    }

    bool readreplayprefix(const char *file, inputrun &run, replayinfo &info, int &known)
    {
        stream *f = openrawfile(path(file, true), "rb");
        if(!f) return false;
        vector<uchar> buf;
        uchar chunk[65536];
        for(size_t n; (n = f->read(chunk, sizeof(chunk))) > 0;) buf.put(chunk, int(n));
        delete f;
        return buf.length() >= 4 && !memcmp(buf.getbuf(), "SBR2", 4) && readsbr2prefix(file, buf, run, info, known);
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
        int fails, warns, missing; // missing: data files this game does not have
        string failed;             // names of the failed checks, comma separated
        verifier() : fails(0), warns(0), missing(0) { failed[0] = '\0'; }

        void pass(const char *name, const char *detail = "") { conoutf("\f0PASS\f7 %s %s", name, detail); }
        void fail(const char *name, const char *detail = "")
        {
            conoutf("\f3FAIL\f7 %s %s", name, detail);
            if(fails++) concatstring(failed, ",");
            concatstring(failed, name);
        }
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
    // The public keys of the witness, built in. A key list in a file (witnesskeys.txt, or the
    // keys argument of replayverify) adds to them.
    static const char * const witnesspinnedkeys[] = { NULL };
    // the signing key of the witness at witnesshost, saved with its channel key
    SVARP(witnesssignkey, "");
    // the signing keys of the witnesses the hub named, newest first, space-separated
    SVARP(hubwitnesssignkeys, "");

    static void loadwitnesskeys(const char *file, vector<uchar> &keys)
    {
        uchar key[32];
        for(int i = 0; witnesspinnedkeys[i]; i++) if(sbr2::fromhex(witnesspinnedkeys[i], key, 32)) keys.put(key, 32);
        if(sbr2::fromhex(witnesssignkey, key, 32)) keys.put(key, 32);
        vector<char *> hubkeys;
        explodelist(hubwitnesssignkeys, hubkeys);
        loopv(hubkeys) if(sbr2::fromhex(hubkeys[i], key, 32)) keys.put(key, 32);
        hubkeys.deletearrays();
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
            if(sbr2::fromhex(s, key, 32)) keys.put(key, 32);
        }
        delete f;
    }

    // set while sauer_resim judges a run for the witness: its earlier verdicts don't count
    static bool ignoreverdicts = false;

    // the trusted key a signature names, or NULL
    static const uchar *signaturekey(const vector<uchar> &keys, const sbr2::signature &s)
    {
        for(int k = 0; k + 32 <= keys.length(); k += 32)
        {
            uchar id[8];
            sbr2::keyid(&keys[k], id);
            if(!memcmp(id, s.keyid, 8)) return &keys[k];
        }
        return NULL;
    }

    // the latest verdict a trusted witness signed over the file's content, "" without one
    static void fileverdict(const char *file, const vector<uchar> &keys, char *out, int outlen)
    {
        out[0] = '\0';
        stream *f = openrawfile(path(file, true), "rb");
        if(!f) return;
        vector<uchar> buf;
        uchar chunk[65536];
        for(size_t n; (n = f->read(chunk, sizeof(chunk))) > 0;) buf.put(chunk, int(n));
        delete f;
        sbr2::reader rd;
        if(!rd.parse(buf.getbuf(), buf.length(), sbr2::KNOWNCHUNKS)) return;
        unsigned long long latestus = 0;
        loopv(rd.chunks)
        {
            const sbr2::chunk &c = *rd.chunks[i];
            sbr2::signature s;
            if(memcmp(c.type, "sIGN", 4) || !s.read(c.data.getbuf(), c.data.length()) || s.scope != sbr2::signature::VERDICT) continue;
            const uchar *key = signaturekey(keys, s);
            if(!key || memcmp(rd.content, s.hash, sbr2::HASHSIZE) || !s.check(key) || s.recvus < latestus) continue;
            latestus = s.recvus;
            if(!sbr2::kvfind(s.extras, "verdict", NULL, out, outlen)) copystring(out, "none", outlen);
        }
    }

    static void verifysignatures(verifier &v, const sbr2::reader &rd, const char *keyfile)
    {
        vector<uchar> keys;
        loadwitnesskeys(keyfile, keys);
        int good = 0, unknown = 0, bad = 0, final = 0, verdicts = 0, rejected = 0, unverified = 0, players = 0;
        uchar player[sbr2::signature::INSTALLKEYID];
        unsigned long long latestus = 0;
        string latest = "";
        loopv(rd.chunks)
        {
            const sbr2::chunk &c = *rd.chunks[i];
            if(memcmp(c.type, "sIGN", 4)) continue;
            sbr2::signature s;
            if(!s.read(c.data.getbuf(), c.data.length())) { bad++; continue; }
            if(ignoreverdicts && s.scope == sbr2::signature::VERDICT) continue;
            const uchar *expected = s.scope != sbr2::signature::SEGMENT ? rd.content :
                                    s.segment < rd.heads.length() / sbr2::HASHSIZE ? &rd.heads[s.segment * sbr2::HASHSIZE] : NULL;
            const uchar *key = signaturekey(keys, s);
            if(!key) { unknown++; continue; }
            if(!expected || memcmp(expected, s.hash, sbr2::HASHSIZE) || !s.check(key)) { bad++; continue; }
            good++;
            if(s.version >= 2)
            {
                if(!players++) memcpy(player, s.installkey, sizeof(player));
                else if(memcmp(player, s.installkey, sizeof(player))) players = -1000;
            }
            if(s.scope == sbr2::signature::FINAL) final++;
            if(s.scope == sbr2::signature::VERDICT)
            {
                verdicts++;
                if(s.recvus >= latestus)
                {
                    latestus = s.recvus;
                    if(!sbr2::kvfind(s.extras, "verdict", NULL, latest, sizeof(latest))) copystring(latest, "none");
                }
            }
        }
        // a run judged again carries every verdict; the latest one counts
        if(verdicts && !strcmp(latest, "unverified")) unverified++;
        else if(verdicts && strcmp(latest, "valid")) rejected++;
        string who = "no player key";
        if(players > 0) { copystring(who, "player "); sbr2::tohex(player, sizeof(player), &who[7]); }
        defformatstring(detail, "%d valid (%d final, %d verdict%s%s), %d unknown key, %d bad, %d trusted keys, %s",
            good, final, verdicts, verdicts ? ", latest " : "", verdicts ? latest : "", unknown, bad, keys.length() / 32, players < 0 ? "signed for different players" : who);
        if(bad || rejected || players < 0) v.fail("signatures", detail);
        else if((!final && !verdicts) || unverified) v.warn("signatures", detail);
        else v.pass("signatures", detail);
    }

    static void verifydata(verifier &v, const inputrun &run)
    {
        int files = 0, mismatched = 0;
        string first = "";
        loopvk(run.attempts) loopv(run.attempts[k]->files)
        {
            const mapdatafile &f = run.attempts[k]->files[i];
            if(f.role > 1) continue;
            files++;
            size_t len = 0;
            char *buf = loadfile(path(f.path, true), &len, false);
            uchar hash[32];
            bool same = false;
            if(buf)
            {
                // a cfg (role 1) is recorded with CR LF read as LF; older replays
                // carry the hash of the raw bytes, so either matches
                loopj(f.role == 1 ? 2 : 1)
                {
                    if(j)
                    {
                        size_t n = 0;
                        loopk(int(len)) if(buf[k] != '\r' || size_t(k+1) >= len || buf[k+1] != '\n') buf[n++] = buf[k];
                        if(n == len) break;
                        len = n;
                    }
                    blake3_hasher h;
                    blake3_hasher_init(&h);
                    blake3_hasher_update(&h, buf, len);
                    blake3_hasher_finalize(&h, hash, sizeof(hash));
                    if(len == f.size && !memcmp(hash, f.hash, sizeof(hash))) { same = true; break; }
                }
                delete[] buf;
            }
            if(!buf) v.missing++;
            if(!same && !mismatched++) copystring(first, f.path);
        }
        if(!files) { v.fail("data", "no file hashes (converted or older replay)"); return; }
        defformatstring(detail, "%d files%s%s%s", files, mismatched ? ", differs or missing: " : " match", first, v.missing ? " (some missing)" : "");
        v.check(!mismatched, "data", detail);
    }

    static void verifyrules(verifier &v, const inputrun &run, const replayinfo &info)
    {
        bool segmented = info.splices > 0;
        bool cleanstart = true;
        loopv(run.attempts) if(run.attempts[i]->snapshot.length() || run.attempts[i]->starttick) cleanstart = false;
        if(segmented) { defformatstring(d, "%d state loads", info.splices); v.warn("segmented", d); }
        else
        {
            // flagrun and race replays hold one life and start from its first tick
            bool life = info.flagrun || info.race;
            v.check(cleanstart || life, "start", cleanstart ? "map start" : life ? "life start" : "starts from a saved state");
        }

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
            formatstring(d, "%d-%d", speedmin, speedmax);
            v.check(speedmin == 100 && speedmax == 100, "gamespeed", d);
        }
        slowmo = slowmo || tellvalue(run, "fEAT", "slowmosp", 0);
        v.check(!slowmo, "slow motion", slowmo ? "slowmosp on" : "off");
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

        if(const runchunk *feat = findtells(run, "fEAT"))
        {
            string category = "", on = "";
            sbr2::kvfind(feat->data, "category", NULL, category, sizeof(category));
            sbr2::bytesreader r(feat->data.getbuf(), feat->data.length());
            int n = int(r.var());
            loopi(n)
            {
                string key;
                if(!r.str(key, sizeof(key))) break;
                int type = r.u8();
                if(type == sbr2::VALUE_BOOL) { if(r.u8()) { if(on[0]) concatstring(on, " "); concatstring(on, key); } }
                else if(type == sbr2::VALUE_STR) { unsigned long long len = r.var(); if(len > (unsigned long long)r.left()) break; r.p += len; }
                else break;
            }
            conoutf("INFO category %s; features on: %s", category, on[0] ? on : "none");
        }
        if(const runchunk *devs = findtells(run, "dEVS"))
        {
            static const char * const kinds[] = { "other", "keyboard", "mouse" };
            sbr2::bytesreader r(devs->data.getbuf(), devs->data.length());
            int n = int(r.var());
            loopi(n)
            {
                int kind = r.u8(), vendor = int(r.var()), product = int(r.var());
                string transport, name;
                if(!r.str(transport, sizeof(transport)) || !r.str(name, sizeof(name))) break;
                conoutf("INFO device %s %04x:%04x %s %s", kinds[kind <= 2 ? kind : 0], vendor, product, transport, name);
            }
        }
        long long losses = tellvalue(run, "wIND", "focuslosses", 0), minimizes = tellvalue(run, "wIND", "minimizes", 0);
        formatstring(d, "%lld focus losses, %lld minimizes", losses, minimizes);
        if(losses || minimizes) v.warn("window", d);
        else v.pass("window", d);
    }

    static bool replayverify(const char *file, const char *keyfile, verifier &v)
    {
        stream *f = openrawfile(path(file, true), "rb");
        if(!f) { conoutf(CON_ERROR, "replayverify: could not open %s", file); return false; }
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
        return true;
    }

    static void replayverifycmd(const char *file, const char *keys)
    {
        verifier v;
        intret(replayverify(file, keys, v) ? v.fails : -1);
    }
    ICOMMAND(replayverify, "ss", (char *file, char *keys), replayverifycmd(file, keys));

    // the leaderboard rules of replayverify for sauer_resim: false if the file can't be read
    bool replayrules(const char *file, const char *keys, int &fails, int &warns, int &missing, char *failed)
    {
        verifier v;
        ignoreverdicts = true;
        bool read = replayverify(file, keys, v);
        ignoreverdicts = false;
        if(!read) return false;
        fails = v.fails;
        warns = v.warns;
        missing = v.missing;
        copystring(failed, v.failed, MAXSTRLEN);
        return true;
    }

    // Rewrites a replay in the current format in memory and checks that it can be streamed: the
    // bytes written after n segments are the first bytes of the whole file for every n, and
    // reading the file back and writing it again gives the same bytes. Prints one line; the
    // result is 1 if every check passed. With out set, the rewritten file is saved there.
    static bool replaystreamcheck(const char *file, const char *out)
    {
        inputrun run;
        replayinfo info;
        if(!readreplay(file, &run, info) || run.empty()) { conoutf(CON_ERROR, "STREAMCHECK %s: could not read", file); return false; }
        info.live = false;
        sbr2stream wholestream;
        buildsbr2(wholestream, run, info);
        const sbr2::writer &whole = wholestream.out;
        int segments = whole.segments, bad = -1;
        for(int n = 0; n <= segments && bad < 0; n++)
        {
            sbr2stream partstream;
            buildsbr2(partstream, run, info, n);
            const sbr2::writer &part = partstream.out;
            if(part.segments != n || part.out.length() > whole.out.length() || memcmp(part.out.getbuf(), whole.out.getbuf(), part.out.length())) bad = n;
        }
        if(bad >= 0) { conoutf(CON_ERROR, "STREAMCHECK %s: FAIL prefix after %d segments differs", file, bad); return false; }
        if(info.version == sbr2::VERSION)
        {
            stream *in = openrawfile(path(file, true), "rb");
            vector<uchar> orig;
            if(in) { uchar chunk[65536]; for(size_t n; (n = in->read(chunk, sizeof(chunk))) > 0;) orig.put(chunk, int(n)); delete in; }
            int len = whole.out.length();
            if(orig.length() < len || memcmp(orig.getbuf(), whole.out.getbuf(), len))
            {
                conoutf(CON_ERROR, "STREAMCHECK %s: FAIL rewriting a current-version file changed its bytes", file);
                return false;
            }
        }
        {
            sbr2::reader whole1;
            sbr2::streamreader pieces;
            bool ok = whole1.parse(whole.out.getbuf(), whole.out.length(), sbr2::KNOWNCHUNKS);
            for(int pos = 0, step = 1; ok && pos < whole.out.length(); pos += step, step = step % 997 + 13)
                ok = pieces.feed(whole.out.getbuf() + pos, min(step, whole.out.length() - pos), sbr2::KNOWNCHUNKS);
            if(!ok || !pieces.ended || pieces.chunks.length() != whole1.chunks.length() || pieces.heads.length() != whole1.heads.length() ||
               memcmp(pieces.heads.getbuf(), whole1.heads.getbuf(), whole1.heads.length()) || memcmp(pieces.content, whole1.content, sbr2::HASHSIZE))
            {
                conoutf(CON_ERROR, "STREAMCHECK %s: FAIL reading the file in pieces differs: %s", file, pieces.error);
                return false;
            }
        }
        inputrun again;
        replayinfo againinfo;
        if(!readsbr2(file, whole.out, &again, againinfo)) { conoutf(CON_ERROR, "STREAMCHECK %s: FAIL rewritten file does not read", file); return false; }
        againinfo.live = false;
        sbr2stream twicestream;
        buildsbr2(twicestream, again, againinfo);
        const sbr2::writer &twice = twicestream.out;
        if(twice.out.length() != whole.out.length() || memcmp(twice.out.getbuf(), whole.out.getbuf(), whole.out.length()))
        {
            conoutf(CON_ERROR, "STREAMCHECK %s: FAIL read back and rewritten, the bytes differ", file);
            return false;
        }
        if(out && out[0])
        {
            stream *o = openrawfile(path(out, true), "wb");
            if(!o || o->write(whole.out.getbuf(), whole.out.length()) != size_t(whole.out.length())) { delete o; conoutf(CON_ERROR, "STREAMCHECK %s: could not write %s", file, out); return false; }
            delete o;
        }
        stream *f = openrawfile(path(file, true), "rb");
        int size = f ? int(f->size()) : 0;
        delete f;
        conoutf("STREAMCHECK %s: ok v%d -> v%d, %d segments, %d -> %d bytes", file, info.version, sbr2::VERSION, segments, size, whole.out.length());
        return true;
    }
    ICOMMAND(replaystreamcheck, "ss", (char *file, char *out), intret(replaystreamcheck(file, out) ? 1 : 0));

    static bool sameevent(const inputevent &x, const inputevent &y)
    {
        return x.tick == y.tick && x.type == y.type && x.arg == y.arg && x.source == y.source && x.controls == y.controls &&
               x.cursorx == y.cursorx && x.cursory == y.cursory && !strcmp(x.cmd ? x.cmd : "", y.cmd ? y.cmd : "");
    }

    static bool samehash(const tickhash &x, const tickhash &y)
    {
        return x.tick == y.tick && x.combined == y.combined && !memcmp(x.groups, y.groups, sizeof(x.groups));
    }

    // Reads the file as it stands after each of its segments: each front must hold every event
    // and hash before its known tick and nothing the whole file doesn't hold.
    static bool replayprefixcheck(const char *file)
    {
        inputrun run;
        replayinfo info;
        if(!readreplay(file, &run, info) || run.empty()) { conoutf(CON_ERROR, "PREFIXCHECK %s: could not read", file); return false; }
        info.live = false;
        sbr2stream wholestream;
        buildsbr2(wholestream, run, info);
        int segments = wholestream.out.segments;
        for(int n = 0; n <= segments; n++)
        {
            sbr2stream part;
            buildsbr2(part, run, info, n);
            inputrun front;
            replayinfo frontinfo;
            int known = -1;
            string why = "";
            if(!readsbr2prefix(file, part.out.out, front, frontinfo, known)) copystring(why, "does not read");
            else if(front.attempts.length() > run.attempts.length()) copystring(why, "has more attempts");
            else loopvk(front.attempts)
            {
                const inputattempt &f = *front.attempts[k], &w = *run.attempts[k];
                int limit = k == front.attempts.length()-1 ? known : INT_MAX;
                bool ok = f.events.length() <= w.events.length() && f.hashes.length() <= w.hashes.length();
                loopv(f.events) if(ok && !sameevent(f.events[i], w.events[i])) ok = false;
                loopv(f.hashes) if(ok && !samehash(f.hashes[i], w.hashes[i])) ok = false;
                if(ok && f.events.length() < w.events.length() && w.events[f.events.length()].tick < limit) ok = false;
                if(ok && f.hashes.length() < w.hashes.length() && w.hashes[f.hashes.length()].tick < limit) ok = false;
                if(!ok) { formatstring(why, "attempt %d differs (known tick %d, %d/%d events, %d/%d hashes)", k, known, f.events.length(), w.events.length(), f.hashes.length(), w.hashes.length()); break; }
            }
            if(why[0]) { conoutf(CON_ERROR, "PREFIXCHECK %s: FAIL after %d segments: front %s", file, n, why); return false; }
        }
        conoutf("PREFIXCHECK %s: ok, %d segments, %d attempts", file, segments, run.attempts.length());
        return true;
    }
    ICOMMAND(replayprefixcheck, "s", (char *file), intret(replayprefixcheck(file) ? 1 : 0));

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
        info.date = first.date ? first.date : uint(time(NULL));
        info.live = true;
        copystring(info.client, SPEEDBRATEN_CLIENT);
        copystring(info.player, first.player[0] ? first.player : player1->name);
        info.mode = first.mode;
        info.flagrun = first.flagrun;
        info.race = first.race;
        info.finished = finished ? 1 : 0;
        info.runtime = finished ? runtime : 0;
        info.ticks = info.splices = 0;
        loopv(run.attempts) { info.ticks += run.attempts[i]->ticks - run.attempts[i]->starttick; info.splices += run.attempts[i]->splices.length(); }
        copystring(info.map, first.map);
    }

    // Online witnessing: with witnesshost set, the client keeps a channel to the witness, takes
    // the seed of every map start from it and streams each run's replay file while it is played.
    // The witness signs segment heads as they close and the content hash at the end; the
    // signatures go into the saved file after END.
    SVARP(witnesshost, "");
    VARP(witnessport, 1, WITNESSPORT, 65535);
    SVARP(witnesskey, ""); // the witness's channel public key, 64 hex digits
    // The keys are learned from the witness the first time an address is used ("host:port" in
    // witnesskeyfor). If that witness later shows a different key, the client stays away from
    // it until the player trusts the new key (witnesstrustnewkey).
    SVARP(witnesskeyfor, "");
    // Without witnesshost, the client uses the witness the hub names, with the keys the hub sent.
    SVARP(hubwitnesshost, "");
    VARP(hubwitnessport, 1, WITNESSPORT, 65535);
    SVARP(hubwitnesskey, "");

    void witnessfromhub(const char *host, int port, const uchar *channel, const uchar *signing)
    {
        string hex;
        setsvar("hubwitnesshost", host);
        setvar("hubwitnessport", clamp(port, 1, 65535));
        sbr2::tohex(channel, 32, hex);
        setsvar("hubwitnesskey", hex);
        sbr2::tohex(signing, 32, hex);
        vector<char *> old;
        explodelist(hubwitnesssignkeys, old);
        vector<char> keys;
        keys.put(hex, strlen(hex));
        for(int i = 0, n = 1; i < old.length() && n < 8; i++) if(strcmp(old[i], hex)) { keys.add(' '); keys.put(old[i], strlen(old[i])); n++; }
        keys.add('\0');
        old.deletearrays();
        setsvar("hubwitnesssignkeys", keys.getbuf());
    }

    // the witness in use: the one set by hand, else the hub's; fromhub says which
    static const char *witnessaddress(int &port, bool &fromhub)
    {
        fromhub = !witnesshost[0] && hubwitnesshost[0];
        port = fromhub ? hubwitnessport : witnessport;
        return fromhub ? hubwitnesshost : witnesshost;
    }

    enum { WITNESSRETRY = 10000, WITNESSSEEDWAIT = 3000, WITNESSPIECE = 60000 };

    struct livestream
    {
        int runid, attempts, sent, lasttick, starttick;
        bool life;
        sbr2stream s;
        replayinfo info;
        const inputattempt *data;
        vector<vector<uchar> > signatures;

        livestream() : runid(0), attempts(0), sent(0), lasttick(0), starttick(0), life(false), data(NULL) {}
    };
    static livestream *live = NULL;
    // the console hears of a lost or unreachable witness once, and again when it is back
    static bool witnesslost = false, witnessreported = false;
    static int witnesslaststate = WITNESS_IDLE;
    static int nextrunid = 1, witnessretry = 0;
    static vector<uint> witnessseeds;
    static uint witnessbeat = 0; // the newest BEAT, returned in RUNSTART and TICK
    static string witnesstarget = "";
    struct awaitingsign { int runid; bool finished; string file; };
    static vector<awaitingsign> awaitingsigns;

    static void appendsignature(const char *file, const vector<uchar> &payload)
    {
        stream *f = openrawfile(path(file, true), "ab");
        if(!f) return;
        vector<uchar> chunk;
        sbr2::appendchunk(chunk, "sIGN", payload);
        f->write(chunk.getbuf(), chunk.length());
        delete f;
    }

    static void witnessmessages()
    {
        int type;
        vector<uchar> p;
        while(witnessreceive(type, p))
        {
            sbr2::bytesreader r(p.getbuf(), p.length());
            if(type < 0)
            {
                r.u8();
                uchar skip[40];
                r.bytes(skip, 40);
                uint seed = r.u32();
                witnessseeds.setsize(0);
                witnessbeat = 0;
                if(r.ok) witnessseeds.add(seed);
                if(witnesslost) conoutf("\f0witness connected again%s", inputattemptwitnessed() || !runtimerkindname() ? "" : ": load the map again to have its runs witnessed");
                else logoutf("witness connected");
                witnesslost = false;
                continue;
            }
            if(type == WM_SEED) { uint seed = r.u32(); if(r.ok) witnessseeds.add(seed); continue; }
            if(type == WM_BEAT) { uint beat = uint(r.var()); if(r.ok) witnessbeat = beat; continue; }
            int runid = int(r.var());
            if(!r.ok) continue;
            if(type == WM_SIGN)
            {
                vector<uchar> payload;
                payload.put(r.p, r.left());
                if(live && live->runid == runid) { live->signatures.add(payload); continue; }
                sbr2::signature sig;
                bool ok = sig.read(payload.getbuf(), payload.length());
                loopv(awaitingsigns) if(awaitingsigns[i].runid == runid)
                {
                    appendsignature(awaitingsigns[i].file, payload);
                    if(ok && sig.scope == sbr2::signature::FINAL) logoutf("witness signed %s", awaitingsigns[i].file);
                    if(ok && sig.scope == sbr2::signature::VERDICT)
                    {
                        string verdict = "", reason = "";
                        sbr2::kvfind(sig.extras, "verdict", NULL, verdict, sizeof(verdict));
                        sbr2::kvfind(sig.extras, "reason", NULL, reason, sizeof(reason));
                        if(!strcmp(verdict, "valid")) logoutf("witness verdict on %s: valid", awaitingsigns[i].file);
                        else if(awaitingsigns[i].finished) conoutf(CON_ERROR, "the witness judged %s %s%s%s", awaitingsigns[i].file, verdict, reason[0] ? ": " : "", reason);
                        else logoutf("the witness judged %s %s%s%s", awaitingsigns[i].file, verdict, reason[0] ? ": " : "", reason);
                        awaitingsigns.remove(i);
                    }
                    break;
                }
            }
            else if(type == WM_RUNERROR || type == WM_RUNSTOP)
            {
                string why = "";
                r.str(why, sizeof(why));
                // a flagrun or race life the witness stopped watching ends at the player's next key
                // press, so the run they go on to play is a new life the witness watches
                bool life = type == WM_RUNSTOP && live && live->runid == runid && live->life;
                conoutf(CON_ERROR, type == WM_RUNSTOP ? "the witness stopped watching this run: %s%s" : "the witness refused this run: %s%s", why,
                        life ? " - press a key to respawn and be witnessed again" : "");
                if(life) inputrespawnonkey();
                if(live && live->runid == runid) DELETEP(live);
            }
        }
    }

    static bool witnessasking = false, witnesschecking = false, witnesskeychanged = false;
    static uchar witnessnewkeys[64];
    static string witnesschangedat = ""; // the address whose key changed

    static void savewitnesskeys(const char *addr, const uchar *channel, const uchar *signing)
    {
        string hex;
        sbr2::tohex(channel, 32, hex);
        setsvar("witnesskey", hex);
        sbr2::tohex(signing, 32, hex);
        setsvar("witnesssignkey", hex);
        setsvar("witnesskeyfor", addr);
    }

    // keeps the channel open while there is a witness to use, and reads what the witness sent
    void witnessupdate()
    {
        int port;
        bool fromhub;
        const char *host = witnessaddress(port, fromhub);
        defformatstring(addr, "%s:%d", host, port);
        // a key set by hand, before keys were learned, belongs to the address set with it
        if(!witnesskeyfor[0] && witnesskey[0] && witnesshost[0]) setsvar("witnesskeyfor", addr);
        uchar key[32], channel[32], signing[32];
        bool known = fromhub ? sbr2::fromhex(hubwitnesskey, key, 32) : !strcmp(witnesskeyfor, addr) && sbr2::fromhex(witnesskey, key, 32);
        if(witnesslearnedkeys(channel, signing) && !fromhub)
        {
            if(witnessasking || !known)
            {
                savewitnesskeys(addr, channel, signing);
                conoutf("\f0witness keys saved for %s", addr);
                known = sbr2::fromhex(witnesskey, key, 32);
                witnessretry = totalmillis;
            }
            else if(memcmp(channel, key, 32))
            {
                witnesskeychanged = true;
                copystring(witnesschangedat, addr);
                memcpy(witnessnewkeys, channel, 32);
                memcpy(witnessnewkeys + 32, signing, 32);
                conoutf(CON_ERROR, "the witness at %s shows a different key than the one saved: check it in the speedrun menu (witness tab)", addr);
            }
            witnessasking = witnesschecking = false;
        }
        if(witnesskeychanged && strcmp(witnesschangedat, addr)) witnesskeychanged = false;
        const char *target = host[0] && !witnesskeychanged ? addr : "";
        int state = witnessstate();
        if(strcmp(target, witnesstarget))
        {
            copystring(witnesstarget, target);
            witnessseeds.setsize(0);
            DELETEP(live);
            witnessconnect("", port, NULL, SPEEDBRATEN_CLIENT);
            witnessasking = witnesschecking = false;
            witnessretry = totalmillis;
            state = witnessstate();
        }
        // with the wrong key the witness can't read message 1 and hangs up
        const char *why = witnesserror();
        bool refused = state == WITNESS_CLOSED && known && !fromhub && !witnesschecking && (!strncmp(why, "handshake refused", 17) || strstr(why, "during the handshake"));
        if(state == WITNESS_CLOSED && witnesslaststate != WITNESS_CLOSED && witnesstarget[0] && !refused && !witnesschecking)
        {
            if(witnesslaststate == WITNESS_OPEN) conoutf(CON_ERROR, "lost the connection to the witness: %s", why);
            else if(!witnessreported) conoutf(CON_ERROR, "can't reach the witness at %s: %s", addr, why);
            witnessreported = witnesslost = true;
        }
        if(state == WITNESS_OPEN) witnessreported = false;
        witnesslaststate = state;
        if(witnesstarget[0] && (state == WITNESS_IDLE || state == WITNESS_CLOSED) && (totalmillis - witnessretry >= 0 || refused))
        {
            witnessseeds.setsize(0);
            if(!known || refused)
            {
                witnessasking = !known;
                witnesschecking = known;
                witnessconnect(host, port, NULL, SPEEDBRATEN_CLIENT);
            }
            else witnessconnect(host, port, key, SPEEDBRATEN_CLIENT);
            witnessretry = totalmillis + WITNESSRETRY;
        }
        if(state != WITNESS_OPEN && live) DELETEP(live);
        witnessmessages();
    }

    ICOMMAND(witnessstatus, "", (),
    {
        int port;
        bool fromhub;
        int state = witnessstate();
        if(!witnessaddress(port, fromhub)[0]) result("off");
        else if(witnesskeychanged) result("the witness shows a different key");
        else if(witnessasking && state == WITNESS_CONNECTING) result("asking for the witness's keys");
        else if(state == WITNESS_OPEN) result("connected");
        else if(state == WITNESS_CONNECTING) result("connecting");
        else if(state == WITNESS_CLOSED) { defformatstring(why, "not connected: %s", witnesserror()); result(why); }
        else result("not connected");
    });
    // the colour code for witnessstatus: green connected, yellow on the way, red a problem, grey off
    ICOMMAND(witnessstatuscolour, "", (),
    {
        int port;
        bool fromhub;
        int state = witnessstate();
        result(!witnessaddress(port, fromhub)[0] ? "\f4" : witnesskeychanged || state == WITNESS_CLOSED ? "\f3" : state == WITNESS_OPEN ? "\f0" : state == WITNESS_CONNECTING ? "\f2" : "\f4");
    });
    ICOMMAND(witnesskeychanged, "", (), intret(witnesskeychanged ? 1 : 0));
    ICOMMAND(witnessnewkey, "", (), { string hex = ""; if(witnesskeychanged) sbr2::tohex(witnessnewkeys, 32, hex); result(hex); });
    ICOMMAND(witnesstrustnewkey, "", (),
    {
        if(!witnesskeychanged) return;
        defformatstring(addr, "%s:%d", witnesshost, witnessport);
        savewitnesskeys(addr, witnessnewkeys, witnessnewkeys + 32);
        witnesskeychanged = false;
        conoutf("\f0witness keys saved for %s", addr);
    });
    ICOMMAND(witnessforgetkeys, "", (),
    {
        setsvar("witnesskey", "");
        setsvar("witnesssignkey", "");
        setsvar("witnesskeyfor", "");
        witnesskeychanged = false;
        witnesstarget[0] = '\0';
    });

    bool witnessseed(uint &seed)
    {
        int port;
        bool fromhub;
        if(!witnessaddress(port, fromhub)[0]) return false;
        witnessupdate();
        // witnessupdate goes on from a key request to the connection, so a first use waits too
        for(int start = SDL_GetTicks(); witnessseeds.empty() && witnessstate() != WITNESS_IDLE && witnessstate() != WITNESS_CLOSED && int(SDL_GetTicks()) - start < WITNESSSEEDWAIT; )
        {
            witnessslice();
            witnessupdate();
            if(witnessseeds.empty()) SDL_Delay(5);
        }
        if(witnessseeds.empty() || witnessstate() != WITNESS_OPEN) return false;
        seed = witnessseeds.remove(0);
        return true;
    }

    static void witnesssendbytes()
    {
        const vector<uchar> &out = live->s.out.out;
        while(live->sent < out.length())
        {
            int n = min(out.length() - live->sent, int(WITNESSPIECE));
            vector<uchar> p;
            sbr2::byteswriter w(p);
            w.var(live->runid);
            w.bytes(out.getbuf() + live->sent, n);
            if(!witnesssend(WM_DATA, p.getbuf(), p.length())) { DELETEP(live); return; }
            live->sent += n;
        }
    }

    static void sendstop(const char *reason);

    // header: the attempt as the saved file will store it; data: the live recording; a life's
    // stream leaves out everything at or before skiptick
    static int lastfinished = -1;

    // the stream that the last saved replay finished, once; -1 if none
    int witnesstakefinished()
    {
        int runid = lastfinished;
        lastfinished = -1;
        return runid;
    }

    // continues: the run id of the finished stream this run of the same life goes on from
    void witnessrunstart(const inputattempt &header, const inputattempt &data, int skiptick, bool life, int continues)
    {
        // a life whose run never started isn't saved, so its stream ends here
        if(live) sendstop("a new run started");
        if(witnessstate() != WITNESS_OPEN) return;
        live = new livestream;
        live->runid = nextrunid++;
        live->life = life;
        live->attempts = 1;
        live->starttick = header.starttick;
        live->lasttick = header.starttick;
        live->data = &data;
        inputrun first;
        header.copyto(first.add());
        fillreplayinfo(first, false, 0, live->info);
        live->s.meta(first, live->info);
        live->s.beginattempt(0, header, data, live->info.simversion, skiptick);
        vector<uchar> p;
        sbr2::byteswriter w(p);
        w.var(live->runid);
        w.var(continues >= 0 ? WITNESSRUN_CHAIN : life ? WITNESSRUN_LIFE : WITNESSRUN_MAP);
        w.var(header.starttick);
        if(continues >= 0) w.var(continues);
        w.var(witnessbeat);
        if(!witnesssend(WM_RUNSTART, p.getbuf(), p.length())) { DELETEP(live); return; }
        witnesssendbytes();
    }

    // the next map of a chained run
    bool witnessstreaming() { return live != NULL; }

    // Map models decide what players and monsters collide with and see, but a replay's map files don't cover
    // them, and installs differ (models replaced by later Sauerbraten releases, local edits). Each model the
    // map uses gets one entry (role 2): a BLAKE3 over the config and geometry files of its folder and of the
    // folder above it, which the loaders fall back to. Images are left out: which ones load depends on the
    // graphics settings, and the headless judge loads its own set.
    static const char * const modelfileexts[] = { ".cfg", ".md2", ".md3", ".obj", ".iqm", ".md5mesh", ".md5anim" };
    static bool modelfile(const char *name, bool &text)
    {
        int len = strlen(name);
        loopi(sizeof(modelfileexts)/sizeof(modelfileexts[0]))
        {
            int n = strlen(modelfileexts[i]);
            if(len > n && !strcasecmp(name + len - n, modelfileexts[i])) { text = i == 0 || i == 3 || i >= 5; return true; }
        }
        return false;
    }

    static bool sortstrings(const char *a, const char *b) { return strcmp(a, b) < 0; }

    static void hashmodel(const char *name, mapdatafile &f)
    {
        f.role = 2;
        f.size = 0;
        formatstring(f.path, "packages/models/%s", name);
        vector<char *> files;
        string dirs[2];
        copystring(dirs[0], f.path);
        copystring(dirs[1], f.path);
        char *up = strrchr(dirs[1], '/');
        int ndirs = up && up - dirs[1] > int(strlen("packages/models")) ? (*up = '\0', 2) : 1;
        loopi(ndirs)
        {
            vector<char *> names;
            listfiles(dirs[i], NULL, names);
            loopvj(names)
            {
                bool text;
                if(modelfile(names[j], text))
                {
                    defformatstring(full, "%s/%s", dirs[i], names[j]);
                    bool dup = false;
                    loopvk(files) if(!strcmp(files[k], full)) dup = true;
                    if(!dup) files.add(newstring(full));
                }
            }
            names.deletearrays();
        }
        files.sort(sortstrings);
        blake3_hasher h;
        blake3_hasher_init(&h);
        loopv(files)
        {
            size_t len = 0;
            char *buf = loadfile(path(files[i], true), &len, false);
            if(!buf) continue;
            bool text;
            modelfile(files[i], text);
            if(text)
            {
                size_t n = 0;
                loopk(int(len)) if(buf[k] != '\r' || size_t(k+1) >= len || buf[k+1] != '\n') buf[n++] = buf[k];
                len = n;
            }
            uchar lenbytes[4] = { uchar(len), uchar(len >> 8), uchar(len >> 16), uchar(len >> 24) };
            blake3_hasher_update(&h, files[i], strlen(files[i]) + 1);
            blake3_hasher_update(&h, lenbytes, sizeof(lenbytes));
            blake3_hasher_update(&h, buf, len);
            f.size += uint(len);
            delete[] buf;
        }
        blake3_hasher_finalize(&h, f.hash, sizeof(f.hash));
        files.deletearrays();
    }

    void addmodelfiles(vector<mapdatafile> &files)
    {
        static vector<mapdatafile> hashed;
        const vector<extentity *> &ents = entities::getents();
        vector<int> used;
        loopv(ents) if(ents[i]->type == ET_MAPMODEL && ents[i]->attr2 >= 0 && used.find(ents[i]->attr2) < 0) used.add(ents[i]->attr2);
        vector<const char *> names;
        loopv(used)
        {
            const char *name = mapmodelname(used[i]);
            if(!name || !name[0]) continue;
            bool dup = false;
            loopvj(names) if(!strcmp(names[j], name)) dup = true;
            if(dup) continue;
            names.add(name);
            defformatstring(key, "packages/models/%s", name);
            int found = -1;
            loopvj(hashed) if(!strcmp(hashed[j].path, key)) { found = j; break; }
            if(found < 0) { found = hashed.length(); hashmodel(name, hashed.add()); }
            files.add(hashed[found]);
        }
    }

    void witnessnextattempt(const inputattempt &a)
    {
        if(!live) return;
        const inputattempt &prev = *live->data;
        for(int last = live->s.lastsegment(); live->s.seg <= last; ) live->s.writesegment(prev.ticks);
        live->s.beginattempt(live->attempts++, a, a, live->info.simversion);
        live->data = &a;
        live->lasttick = max(a.starttick, 0);
        witnesssendbytes();
    }

    // once per frame while recording: closed segments and a TICK every WITNESSTICKSTEP ticks
    void witnessframe(int curtick)
    {
        if(!live) return;
        const inputattempt &a = *live->data;
        while(curtick >= live->s.segmentend()) live->s.writesegment(-1);
        witnesssendbytes();
        if(!live) return;
        for(int t = (live->lasttick / WITNESSTICKSTEP + 1) * WITNESSTICKSTEP; t <= curtick; t += WITNESSTICKSTEP)
        {
            const tickhash *h = NULL;
            for(int i = a.hashes.length()-1; i >= 0 && a.hashes[i].tick >= t; i--) if(a.hashes[i].tick == t) { h = &a.hashes[i]; break; }
            live->lasttick = t;
            if(!h) continue;
            vector<uchar> p;
            sbr2::byteswriter w(p);
            w.var(live->runid);
            w.var(t);
            w.u32(combinedhash(h->groups, a.hashgroups));
            w.var(witnessbeat);
            if(!witnesssend(WM_TICK, p.getbuf(), p.length())) { DELETEP(live); return; }
        }
    }

    // The spectator stream: the game being played, sent to the hub as views whether or not a
    // witness watches it. A view is a replay file without a trailer that starts at a map start
    // or from a saved state (a loaded one, or a snapshot every minute), with segments of
    // HUBVIEWSEGMENT ticks in every mode so spectators are about a second behind.
    // A view is started during play (every keyframe), so it compresses at a level whose context
    // is quick to set up; the witness stream has to match the saved file's bytes and can't.
    enum { HUBVIEWSEGMENT = 200, HUBVIEWPIECE = 8192, HUBVIEWLEVEL = 9 };

    struct hubview
    {
        int id, attempts, sent, starttick;
        sbr2stream s;
        replayinfo info;
        const inputattempt *data;

        hubview() : id(0), attempts(0), sent(0), starttick(0), data(NULL) {}
    };
    static hubview *view = NULL;
    static int nextviewid = 1;

    static void hubviewsend()
    {
        const vector<uchar> &out = view->s.out.out;
        while(view->sent < out.length())
        {
            int n = min(out.length() - view->sent, int(HUBVIEWPIECE));
            vector<uchar> p;
            sbr2::byteswriter w(p);
            w.var(view->id);
            w.var(view->sent);
            w.bytes(out.getbuf() + view->sent, n);
            if(!hubsend(HM_VIEWDATA, p.getbuf(), p.length())) { DELETEP(view); return; }
            view->sent += n;
        }
    }

    bool hubviewing() { return view != NULL; }
    int hubviewstart() { return view ? view->starttick : 0; }
    void hubviewstop() { DELETEP(view); }

    // header: the attempt as the view starts it (a map start, or a saved state at skiptick)
    void hubviewbegin(const inputattempt &header, const inputattempt &data, int skiptick)
    {
        DELETEP(view);
        if(!hubstreaming()) return;
        view = new hubview;
        view->id = nextviewid++;
        view->attempts = 1;
        view->starttick = max(header.starttick, 0);
        view->data = &data;
        view->s.fixedsegment = HUBVIEWSEGMENT;
        view->s.out.level = HUBVIEWLEVEL;
        inputrun first;
        header.copyto(first.add());
        fillreplayinfo(first, false, 0, view->info);
        vector<uchar> p;
        sbr2::byteswriter w(p);
        w.var(view->id);
        w.str(header.map);
        w.svar(header.mode);
        w.str(hubrunkind());
        if(!hubsend(HM_VIEW, p.getbuf(), p.length())) { DELETEP(view); return; }
        view->s.meta(first, view->info);
        view->s.beginattempt(0, header, data, view->info.simversion, skiptick);
        hubviewsend();
    }

    // the next map of a chained run goes on in the same view
    void hubviewnextattempt(const inputattempt &a)
    {
        if(!view) return;
        const inputattempt &prev = *view->data;
        for(int last = view->s.lastsegment(); view->s.seg <= last; ) view->s.writesegment(prev.ticks);
        view->s.beginattempt(view->attempts++, a, a, view->info.simversion);
        view->data = &a;
        hubviewsend();
    }

    // once per frame while recording: the segments closed since
    void hubviewframe(int curtick)
    {
        if(!view) return;
        if(!hubstreaming()) { DELETEP(view); return; }
        while(curtick >= view->s.segmentend()) view->s.writesegment(-1);
        hubviewsend();
    }

    static void sendstop(const char *reason)
    {
        vector<uchar> p;
        sbr2::byteswriter w(p);
        w.var(live->runid);
        w.str(reason);
        witnesssend(WM_STOP, p.getbuf(), p.length());
        DELETEP(live);
    }

    // a stream whose run never started ends without telling the player
    void witnessdrop(const char *reason)
    {
        if(live) sendstop(reason);
    }

    void witnessstop(const char *reason)
    {
        if(!live) return;
        conoutf("\f2witness: this run is no longer witnessed (%s)", reason);
        sendstop(reason);
    }

    // The saved replay of the streamed run: finishes the stream with the run's end and trailer
    // and returns its bytes if they are the file's bytes; otherwise the stream ends unsigned.
    static bool witnessfinish(const inputrun &run, const replayinfo &info, sbr2stream &whole)
    {
        if(!live) return false;
        if(run.attempts.length() != live->attempts || run.attempts[0]->starttick != live->starttick) { witnessstop("a different run was saved"); return false; }
        const inputattempt &a = *live->data;
        for(int last = live->s.lastsegment(); live->s.seg <= last; ) live->s.writesegment(a.ticks);
        vector<runchunk *> tells;
        if(info.live) tellsencode(run, info, tells);
        else loopv(run.tells) { runchunk *c = tells.add(new runchunk); memcpy(c->type, run.tells[i]->type, sizeof(c->type)); c->late = run.tells[i]->late; c->data = run.tells[i]->data; }
        live->s.trailer(run, info, &tells);
        buildsbr2(whole, run, info, -1, &tells);
        tells.deletecontents();
        const vector<uchar> &mine = live->s.out.out, &want = whole.out.out;
        if(mine.length() != want.length() || memcmp(mine.getbuf(), want.getbuf(), want.length()))
        {
            conoutf(CON_ERROR, "witness: the streamed replay differs from the saved one");
            witnessstop("the stream differs from the saved replay");
            return false;
        }
        witnesssendbytes();
        return live != NULL;
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
            sbr2stream whole;
            if(witnessfinish(run, info, whole)) DELETEP(live);
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
        sbr2stream whole;
        bool written;
        if(witnessfinish(finished ? traced : run, info, whole))
        {
            vector<uchar> bytes(live->s.out.out);
            loopv(live->signatures) sbr2::appendchunk(bytes, "sIGN", live->signatures[i]);
            stream *f = openrawfile(path(file, true), "wb");
            written = f && f->write(bytes.getbuf(), bytes.length()) == size_t(bytes.length());
            delete f;
            memcpy(content, live->s.out.content, sizeof(content));
            awaitingsign &w = awaitingsigns.add();
            w.runid = lastfinished = live->runid;
            w.finished = finished;
            copystring(w.file, file);
            DELETEP(live);
        }
        else written = writereplay(file, finished ? traced : run, info, content);
        if(started) statsrecord(run, info, written ? file : NULL, written ? content : NULL);
        if(!written) { conoutf(CON_ERROR, "could not write %s", file); return; }
        if(finished) conoutf("\f2saved replay %s", file);
        else rotatereplays();
    }

    // Replay browser: replayscan caches the headers of all replay files, newest first.
    struct replayentry
    {
        string file, verdict; // verdict: the latest a trusted witness signed, "" without one
        bool kept, pb, assisted;
        int size;
        replayinfo info;
    };
    static vector<replayentry> replaylist;
    static vector<uchar> replaylistkeys;

    MOD(SVARP, replayfilter, "");
    MOD(VARP, replaysort, 0, 0, 1);
    MOD(VARP, replayfilterkind, 0, 0, 4);   // 0 all, 1 sp, 2 dmsp, 3 flagrun, 4 race
    MOD(VARP, replayfilterresult, 0, 0, 2); // 0 all, 1 finished, 2 unfinished
    MOD(VARP, replayfilterseg, 0, 0, 1);
    MOD(VARP, replayfilterkept, 0, 0, 1);
    MOD(VARP, replayfilterpb, 0, 0, 1);
    MOD(VARP, replayfilterassisted, 0, 0, 2); // 0 all, 1 unassisted, 2 assisted
    MOD(VARP, replayfilterflagmode, 0, 0, 3); // with replayfilterkind 3: 0 all, 1 effic, 2 insta, 3 weaponless

    // the kind numbering of replayfilterkind and statskind; 0 for runs of no listed kind
    static int replaykindof(const replayinfo &r)
    {
        if(r.race) return 4;
        if(r.flagrun) return 3;
        if(m_check(r.mode, M_DMSP)) return 2;
        if(m_check(r.mode, M_CLASSICSP)) return 1;
        return 0;
    }

    // the numbering of replayfilterflagmode and statsflagmode
    int flagrunmodeof(int mode) { return m_check(mode, M_EFFICIENCY) ? 1 : m_check(mode, M_INSTA) ? 2 : 3; }

    static bool replaylisted(const replayinfo &r, bool kept)
    {
        if(replayfilterkind && replaykindof(r) != replayfilterkind) return false;
        if(replayfilterkind == 3 && replayfilterflagmode && flagrunmodeof(r.mode) != replayfilterflagmode) return false;
        if(replayfilterresult && (r.finished != 0) != (replayfilterresult == 1)) return false;
        if(replayfilterseg && r.splices <= 0) return false;
        if(replayfilterkept && !kept) return false;
        return true;
    }

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

    static vector<char *> pbfiles, assistedfiles;

    static bool listedin(const vector<char *> &files, const char *file)
    {
        loopv(files) if(!strcmp(files[i], file)) return true;
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
            if(!readreplay(e.file, NULL, e.info) || !containsnocase(e.info.map, replayfilter) || !replaylisted(e.info, kept)) continue;
            e.pb = listedin(pbfiles, e.file);
            e.assisted = listedin(assistedfiles, e.file);
            if(replayfilterpb && !e.pb) continue;
            if(replayfilterassisted && e.assisted != (replayfilterassisted == 2)) continue;
            e.kept = kept;
            fileverdict(e.file, replaylistkeys, e.verdict, sizeof(e.verdict));
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
        replaylistkeys.setsize(0);
        loadwitnesskeys(NULL, replaylistkeys);
        pbfiles.deletearrays();
        statspbreplays(pbfiles);
        assistedfiles.deletearrays();
        statsassistedreplays(assistedfiles);
        scanreplaydir("replays", false);
        scanreplaydir("replays/kept", true);
        replaylist.sort(replaysort ? fasterreplay : newerreplay);
        intret(replaylist.length());
    });

    static void formatsize(char *buf, double bytes)
    {
        if(bytes >= 1024*1024) nformatstring(buf, MAXSTRLEN, "%.1f MB", bytes/(1024*1024));
        else nformatstring(buf, MAXSTRLEN, "%.1f KB", bytes/1024);
    }

    // every saved replay, whatever the browser's filters: "count, size"
    static void replaysaved()
    {
        int count = 0;
        double total = 0;
        loopi(2)
        {
            const char *dir = i ? "replays/kept" : "replays";
            vector<char *> files;
            listfiles(dir, "sbr", files);
            loopvj(files)
            {
                defformatstring(file, "%s/%s.sbr", dir, files[j]);
                stream *f = openrawfile(path(file, true), "rb");
                if(!f) continue;
                count++;
                total += f->size();
                delete f;
            }
            files.deletearrays();
        }
        string size, buf;
        formatsize(size, total);
        formatstring(buf, "%d, %s", count, size);
        result(buf);
    }
    COMMAND(replaysaved, "");

    // total size of the scanned replays, e.g. "4.6 MB"
    ICOMMAND(replaytotalsize, "", (),
    {
        double total = 0;
        loopv(replaylist) total += replaylist[i].size;
        string buf;
        if(total >= 1024*1024) formatstring(buf, "%.1f MB", total/(1024*1024));
        else formatstring(buf, "%.1f KB", total/1024);
        result(buf);
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
        else if(!strcmp(field, "pb")) intret(e.pb ? 1 : 0);
        else if(!strcmp(field, "assisted")) intret(e.assisted ? 1 : 0);
        else if(!strcmp(field, "verdict")) result(e.verdict);
        else if(!strcmp(field, "outdated")) intret(r.simversion < REPLAYSIMVERSION ? 1 : 0);
        else result("");
    });

    ICOMMAND(replayplay, "i", (int *i),
    {
        if(!replaylist.inrange(*i) || multiplayer(false)) return;
        inputrun run;
        replayinfo info;
        if(!readreplay(replaylist[*i].file, &run, info)) { conoutf(CON_ERROR, "could not read %s", replaylist[*i].file); return; }
        viewinputrun(run, replaylist[*i].file);
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
