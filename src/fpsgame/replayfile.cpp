// Replay files: a run's recorded input plus metadata, gzip-compressed, in replays/ (and
// replays/kept/ for runs protected from the rolling cleanup of unfinished attempts).
//
// Layout (all integers little endian, "var" = unsigned LEB128, "svar" = zigzag LEB128):
//   "SBRP", var version, u32 date, str client, str player, svar mode, var flagrun,
//   [v3+: var race], [v7+: var splices], [v10+: var run started], var finished, var runtime ms, var ticks, var maps,
//   str first map
//   per map: str map, u32 map crc, u32 seed, str team, svar mode, var flagrun,
//            [v3+: var race], [v5+: var settings, (str name, svar value) per setting],
//            [v6+: var snapshot length, snapshot bytes, var start tick (with a snapshot)],
//            [v7+: var splices, (var tick, var discarded ticks) per splice],
//            [v8+: u32 map cfg crc, 0 without a cfg],
//            [v9+: var ghost samples, (var run tick delta, u32 x, u32 y, u32 z, u32 yaw,
//                  u32 pitch, var flags) per sample],
//            var ticks, var events, events, [v4+: var groups], var hashes, hashes
//   event:   var tick delta, u8 type, then by type: controls (u32 yaw, u32 pitch, u8 packed
//            move/strafe/vertical/jumping/attacking), svar arg (jump/attack/zoom), str
//            (command), u32 cursor x, u32 cursor y, str (gui), svar value, str name (setting)
//   hash:    var tick delta, groups x u32 (6 before v4)
//   str:     var length, bytes

#include "game.h"
#include "inputs.h"

namespace game
{
    static const uchar REPLAYMAGIC[4] = { 'S', 'B', 'R', 'P' };
    // REPLAYSIMVERSION: oldest version recorded with the current simulation
    enum { REPLAYVERSION = 10, REPLAYSIMVERSION = 2, REPLAYHASHSTEP = 10 };

    struct replaywriter
    {
        vector<uchar> buf;

        void u8(int v) { buf.add(uchar(v)); }
        void u32(uint v) { loopi(4) buf.add(uchar(v >> (i*8))); }
        void f32(float f) { uint v; memcpy(&v, &f, 4); u32(v); }
        void var(uint v) { do { uchar c = v & 0x7F; v >>= 7; if(v) c |= 0x80; buf.add(c); } while(v); }
        void svar(int v) { var((uint(v) << 1) ^ uint(v >> 31)); }
        void str(const char *s) { int len = strlen(s); var(len); buf.put((const uchar *)s, len); }
    };

    struct replayreader
    {
        const uchar *p, *end;
        bool ok;

        replayreader(const vector<uchar> &b) : p(b.getbuf()), end(b.getbuf() + b.length()), ok(true) {}

        int u8() { if(p >= end) { ok = false; return 0; } return *p++; }
        uint u32() { uint v = 0; loopi(4) v |= uint(u8()) << (i*8); return v; }
        float f32() { uint v = u32(); float f; memcpy(&f, &v, 4); return f; }
        uint var()
        {
            uint v = 0;
            for(int shift = 0; shift < 35; shift += 7)
            {
                int c = u8();
                v |= uint(c & 0x7F) << shift;
                if(!(c & 0x80)) return v;
            }
            ok = false;
            return 0;
        }
        int svar() { uint v = var(); return int(v >> 1) ^ -int(v & 1); }
        void str(char *dst, int maxlen)
        {
            uint len = var();
            if(len > uint(end - p)) { ok = false; dst[0] = '\0'; return; }
            int n = min(int(len), maxlen-1);
            memcpy(dst, p, n);
            dst[n] = '\0';
            p += len;
        }
        char *newstr()
        {
            uint len = var();
            if(len > uint(end - p)) { ok = false; return newstring(""); }
            char *s = newstring((const char *)p, len);
            p += len;
            return s;
        }
    };

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

    bool writereplay(const char *file, const inputrun &run, const replayinfo &info)
    {
        replaywriter w;
        w.buf.put(REPLAYMAGIC, sizeof(REPLAYMAGIC));
        w.var(REPLAYVERSION);
        w.u32(info.date);
        w.str(info.client);
        w.str(info.player);
        w.svar(info.mode);
        w.var(info.flagrun);
        w.var(info.race);
        w.var(info.splices);
        w.var(info.started != 0 ? 1 : 0);
        w.var(info.finished);
        w.var(info.runtime);
        w.var(info.ticks);
        w.var(run.attempts.length());
        w.str(run.attempts.length() ? run.attempts[0]->map : "");
        loopvk(run.attempts)
        {
            const inputattempt &a = *run.attempts[k];
            w.str(a.map);
            w.u32(a.mapcrc);
            w.u32(a.seed);
            w.str(a.team);
            w.svar(a.mode);
            w.var(a.flagrun);
            w.var(a.race);
            w.var(a.settings.length());
            loopv(a.settings) { w.str(a.settings[i].name); w.svar(a.settings[i].value); }
            w.var(a.snapshot.length());
            if(a.snapshot.length()) { w.buf.put(a.snapshot.getbuf(), a.snapshot.length()); w.var(a.starttick); }
            w.var(a.splices.length());
            loopv(a.splices) { w.var(a.splices[i].tick); w.var(a.splices[i].discarded); }
            w.u32(a.cfgcrc);
            w.var(a.trace.length());
            int lasttick = 0;
            loopv(a.trace)
            {
                const ghostsample &g = a.trace[i];
                w.var(g.tick - lasttick);
                lasttick = g.tick;
                w.f32(g.o.x); w.f32(g.o.y); w.f32(g.o.z); w.f32(g.yaw); w.f32(g.pitch);
                w.var(g.flags);
            }
            w.var(a.ticks);
            w.var(a.events.length());
            int last = 0;
            loopv(a.events)
            {
                const inputevent &e = a.events[i];
                w.var(e.tick - last);
                last = e.tick;
                w.u8(e.type);
                switch(e.type)
                {
                    case INPUT_CONTROLS: w.u32(e.controls.yaw); w.u32(e.controls.pitch); w.u8(packcontrols(e.controls)); break;
                    case INPUT_COMMAND: w.str(e.cmd ? e.cmd : ""); break;
                    case INPUT_GUI: w.u32(e.cursorx); w.u32(e.cursory); w.str(e.cmd ? e.cmd : ""); break;
                    case INPUT_SETTING: w.svar(e.arg); w.str(e.cmd ? e.cmd : ""); break;
                    default: w.svar(e.arg); break;
                }
            }
            vector<const tickhash *> hashes;
            loopv(a.hashes) if(a.hashes[i].tick % REPLAYHASHSTEP == 0 || a.hashes[i].tick == a.ticks) hashes.add(&a.hashes[i]);
            w.var(a.hashgroups);
            w.var(hashes.length());
            last = 0;
            loopv(hashes)
            {
                w.var(hashes[i]->tick - last);
                last = hashes[i]->tick;
                loopj(a.hashgroups) w.u32(hashes[i]->groups[j]);
            }
        }
        stream *f = opengzfile(path(file, true), "wb");
        if(!f) return false;
        f->write(w.buf.getbuf(), w.buf.length());
        delete f;
        return true;
    }

    // With run == NULL only the header is read.
    bool readreplay(const char *file, inputrun *run, replayinfo &info)
    {
        stream *f = opengzfile(path(file, true), "rb");
        if(!f) return false;
        vector<uchar> buf;
        uchar chunk[4096];
        for(;;)
        {
            size_t n = f->read(chunk, sizeof(chunk));
            if(!n) break;
            buf.put(chunk, n);
            if(!run && buf.length() >= 1024) break;
        }
        delete f;
        if(buf.length() < int(sizeof(REPLAYMAGIC)) || memcmp(buf.getbuf(), REPLAYMAGIC, sizeof(REPLAYMAGIC))) return false;
        replayreader r(buf);
        r.p += sizeof(REPLAYMAGIC);
        info.version = r.var();
        if(info.version < 1 || info.version > REPLAYVERSION) return false;
        info.date = r.u32();
        r.str(info.client, sizeof(info.client));
        r.str(info.player, sizeof(info.player));
        info.mode = r.svar();
        info.flagrun = r.var();
        info.race = info.version >= 3 ? r.var() : 0;
        info.splices = info.version >= 7 ? r.var() : 0;
        info.started = info.version >= 10 ? int(r.var()) : -1;
        info.finished = r.var();
        info.runtime = r.var();
        info.ticks = r.var();
        info.maps = r.var();
        r.str(info.map, sizeof(info.map));
        if(!r.ok) return false;
        if(!run) return true;
        if(info.version < REPLAYSIMVERSION) conoutf(CON_ERROR, "replay %s was made with an older build: it will likely desync", file);
        run->clear();
        loopk(info.maps)
        {
            inputattempt &a = run->add();
            r.str(a.map, sizeof(a.map));
            a.mapcrc = r.u32();
            a.seed = r.u32();
            r.str(a.team, sizeof(a.team));
            a.mode = r.svar();
            a.flagrun = r.var();
            a.race = info.version >= 3 ? r.var() : 0;
            if(info.version >= 5)
            {
                int settings = r.var();
                loopi(settings)
                {
                    if(!r.ok) break;
                    inputsetting &s = a.settings.add();
                    r.str(s.name, sizeof(s.name));
                    s.value = r.svar();
                }
            }
            if(info.version >= 6)
            {
                uint len = r.var();
                if(len > uint(r.end - r.p)) { r.ok = false; break; }
                a.snapshot.put(r.p, len);
                r.p += len;
                if(len) a.starttick = r.var();
            }
            if(info.version >= 7)
            {
                int n = r.var();
                loopi(n)
                {
                    if(!r.ok) break;
                    inputsplice &sp = a.splices.add();
                    sp.tick = r.var();
                    sp.discarded = r.var();
                }
            }
            if(info.version >= 8)
            {
                a.cfgcrc = r.u32();
                a.hascfgcrc = true;
            }
            if(info.version >= 9)
            {
                int n = r.var(), tick = 0;
                loopi(n)
                {
                    if(!r.ok) break;
                    ghostsample &g = a.trace.add();
                    tick += r.var();
                    g.tick = tick;
                    g.o.x = r.f32(); g.o.y = r.f32(); g.o.z = r.f32(); g.yaw = r.f32(); g.pitch = r.f32();
                    g.flags = r.var();
                }
            }
            a.ticks = r.var();
            int events = r.var(), tick = 0;
            loopi(events)
            {
                if(!r.ok) break;
                inputevent &e = a.events.add();
                memset(&e, 0, sizeof(e));
                tick += r.var();
                e.tick = tick;
                e.type = r.u8();
                switch(e.type)
                {
                    case INPUT_CONTROLS: e.controls.yaw = r.u32(); e.controls.pitch = r.u32(); unpackcontrols(r.u8(), e.controls); break;
                    case INPUT_COMMAND: e.cmd = r.newstr(); break;
                    case INPUT_GUI: e.cursorx = r.u32(); e.cursory = r.u32(); e.cmd = r.newstr(); break;
                    case INPUT_SETTING: e.arg = r.svar(); e.cmd = r.newstr(); break;
                    default: e.arg = r.svar(); break;
                }
            }
            a.hashgroups = info.version >= 4 ? r.var() : int(DETHASH_LEGACYGROUPS);
            if(a.hashgroups < 1 || a.hashgroups > DETHASH_GROUPS) { r.ok = false; break; }
            int hashes = r.var();
            tick = 0;
            loopi(hashes)
            {
                if(!r.ok) break;
                tickhash &h = a.hashes.add();
                tick += r.var();
                h.tick = tick;
                memset(h.groups, 0, sizeof(h.groups));
                loopj(a.hashgroups) h.groups[j] = r.u32();
            }
            if(!r.ok) break;
        }
        if(!r.ok) { run->clear(); return false; }
        return true;
    }

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
        info.version = REPLAYVERSION;
        info.date = uint(time(NULL));
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
        bool written = writereplay(file, finished ? traced : run, info);
        if(started) statsrecord(run, info, written ? file : NULL);
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
        else if(!strcmp(field, "outdated")) intret(r.version < REPLAYSIMVERSION ? 1 : 0);
        else result("");
    });

    ICOMMAND(replayplay, "i", (int *i),
    {
        if(!replaylist.inrange(*i) || multiplayer(false)) return;
        inputrun run;
        replayinfo info;
        if(!readreplay(replaylist[*i].file, &run, info)) { conoutf(CON_ERROR, "could not read %s", replaylist[*i].file); return; }
        playinputrun(run);
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
                else if(!strcmp(r.cond, "outdated")) match = c.info.version < REPLAYSIMVERSION;
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
