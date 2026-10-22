// Splits: named points of a run, defined per map and run kind in splits/<map>_<kind>.cfg (the
// player's own file in the home dir wins over the one in the override dir). A split fires once
// per run, in any order: entering a zone, picking up or using a given entity, a cubescript
// condition turning true, or a map script calling splithere. Split times are run timer ticks.
// The HUD compares them with the PB run's splits and the best time of each segment.

#include "game.h"
#include "inputs.h"

namespace game
{
    enum { SPLIT_ZONE = 0, SPLIT_PICKUP, SPLIT_TELEPORT, SPLIT_WHEN, SPLIT_SCRIPT };
    static const char * const splittypes[] = { "zone", "pickup", "teleport", "when", "script" };

    struct splitdef
    {
        string name;
        int type, ent;
        vec lo, hi;
        string expr;
    };
    static vector<splitdef> defs;

    struct splithit
    {
        int def, tick, millis; // millis: totalmillis when it fired, for the HUD fade
    };
    static vector<splithit> hits;

    MOD(VARP, splithud, 0, 1, 1);
    MOD(VARP, splitlist, 0, 0, 1);
    MOD(VARP, splitshowzones, 0, 0, 1);
    // ms the split lines stay after the last split before fading out; 0 keeps them
    MOD(VARP, splitfade, 0, 3000, 600000);
    enum { SPLITFADEMS = 500 };
    // the run kind whose splits are edited in edit mode or while no run is timed: sp, flagrun, race
    MOD(VARP, spliteditkind, 0, 1, 2);
    static const char * const editkinds[] = { "sp", "flagrun", "race" };

    static string defskey = "", splitkind = "", splitmap = "";
    static int lasttick = -1;

    static int finddef(const char *name)
    {
        loopv(defs) if(!strcmp(defs[i].name, name)) return i;
        return -1;
    }

    static splitdef &adddef(const char *name, int type)
    {
        int i = finddef(name);
        splitdef &d = i >= 0 ? defs[i] : defs.add();
        copystring(d.name, name);
        d.type = type;
        d.ent = -1;
        d.lo = d.hi = vec(0, 0, 0);
        d.expr[0] = '\0';
        return d;
    }

    // definitions, as written in a split file
    static void splitzone(const char *name, float x1, float y1, float z1, float x2, float y2, float z2)
    {
        splitdef &d = adddef(name, SPLIT_ZONE);
        d.lo = vec(min(x1, x2), min(y1, y2), min(z1, z2));
        d.hi = vec(max(x1, x2), max(y1, y2), max(z1, z2));
    }
    ICOMMAND(splitzone, "sffffff", (char *name, float *x1, float *y1, float *z1, float *x2, float *y2, float *z2), splitzone(name, *x1, *y1, *z1, *x2, *y2, *z2));
    ICOMMAND(splitpickup, "si", (char *name, int *ent), adddef(name, SPLIT_PICKUP).ent = *ent);
    ICOMMAND(splitteleport, "si", (char *name, int *ent), adddef(name, SPLIT_TELEPORT).ent = *ent);
    ICOMMAND(splitwhen, "ss", (char *name, char *expr), copystring(adddef(name, SPLIT_WHEN).expr, expr));
    ICOMMAND(splitscript, "s", (char *name), adddef(name, SPLIT_SCRIPT));

    static void splitfilename(char *dst, const char *map, const char *kind)
    {
        string m;
        copystring(m, map);
        for(char *c = m; *c; c++) if(*c == '/' || *c == '\\') *c = '_';
        nformatstring(dst, MAXSTRLEN, "splits/%s_%s.cfg", m, kind);
    }

    static void loaddefs(const char *map, const char *kind)
    {
        defs.setsize(0);
        copystring(splitmap, map);
        copystring(splitkind, kind);
        string file;
        splitfilename(file, map, kind);
        defformatstring(home, "%s%s", homedir, file);
        if(homedir[0] && fileexists(home, "r")) execfile(home, false);
        else
        {
            defformatstring(shipped, "%s%s", overridedir, file);
            if(overridedir[0] && fileexists(shipped, "r")) execfile(shipped, false);
        }
    }

    ICOMMAND(splitsave, "", (),
    {
        if(!splitmap[0]) { conoutf(CON_ERROR, "splitsave: no map"); return; }
        string file;
        splitfilename(file, splitmap, splitkind);
        stream *f = openutf8file(path(file, true), "w");
        if(!f) { conoutf(CON_ERROR, "splitsave: could not write %s", file); return; }
        f->printf("// splits of %s (%s), any order\n", splitmap, splitkind);
        loopv(defs)
        {
            const splitdef &d = defs[i];
            switch(d.type)
            {
                case SPLIT_ZONE: f->printf("splitzone %s %g %g %g %g %g %g\n", escapestring(d.name), d.lo.x, d.lo.y, d.lo.z, d.hi.x, d.hi.y, d.hi.z); break;
                case SPLIT_PICKUP: f->printf("splitpickup %s %d\n", escapestring(d.name), d.ent); break;
                case SPLIT_TELEPORT: f->printf("splitteleport %s %d\n", escapestring(d.name), d.ent); break;
                case SPLIT_WHEN: f->printf("splitwhen %s [%s]\n", escapestring(d.name), d.expr); break;
                case SPLIT_SCRIPT: f->printf("splitscript %s\n", escapestring(d.name)); break;
            }
        }
        delete f;
        conoutf("saved %d splits to %s", defs.length(), file);
    });

    // editing: a zone from the selection, the item or teleport under the cursor, a script split
    // the edit tools never replace an existing split
    static bool newsplitname(const char *cmd, const char *name)
    {
        if(!name[0]) { conoutf(CON_ERROR, "%s: give the split a name", cmd); return false; }
        if(finddef(name) >= 0) { conoutf(CON_ERROR, "%s: a split named %s exists; rename or delete it first", cmd, name); return false; }
        return true;
    }

    static bool splitaddzone(const char *name)
    {
        vec lo, hi;
        if(!newsplitname("splitaddzone", name)) return false;
        if(!editmode || !getselbox(lo, hi)) { conoutf(CON_ERROR, "splitaddzone: select an area in edit mode first"); return false; }
        if(lo.x >= hi.x || lo.y >= hi.y || lo.z >= hi.z) { conoutf(CON_ERROR, "splitaddzone: the selection is empty; drag a box first"); return false; }
        splitdef &d = adddef(name, SPLIT_ZONE);
        d.lo = lo;
        d.hi = hi;
        conoutf("added zone split %s (%s splits of %s; splitsave to keep it)", name, splitkind, splitmap);
        return true;
    }
    // the add commands return 1 when the split was added
    ICOMMAND(splitaddzone, "s", (char *name), intret(splitaddzone(name) ? 1 : 0));
    ICOMMAND(splitaddscript, "s", (char *name),
    {
        if(!newsplitname("splitaddscript", name)) { intret(0); return; }
        adddef(name, SPLIT_SCRIPT);
        conoutf("added script split %s (the map script fires it with splithere; splitsave to keep it)", name);
        intret(1);
    });
    ICOMMAND(splitaddent, "s", (char *name),
    {
        const vector<extentity *> &ents = entities::getents();
        int n = getenthover();
        intret(0);
        if(!newsplitname("splitaddent", name)) return;
        if(!editmode || !ents.inrange(n)) { conoutf(CON_ERROR, "splitaddent: point at an item or teleport in edit mode first"); return; }
        int type = ents[n]->type;
        if(type == TELEPORT) adddef(name, SPLIT_TELEPORT).ent = n;
        else if(type >= I_SHELLS && type <= I_QUAD) adddef(name, SPLIT_PICKUP).ent = n;
        else { conoutf(CON_ERROR, "splitaddent: entity %d is not an item or teleport", n); return; }
        conoutf("added %s split %s on entity %d (splitsave to keep it)", type == TELEPORT ? "teleport" : "pickup", name, n);
        intret(1);
    });
    ICOMMAND(splitremove, "i", (int *i), { if(defs.inrange(*i)) defs.remove(*i); });
    ICOMMAND(splitrename, "is", (int *i, char *name), { if(defs.inrange(*i) && name[0] && finddef(name) < 0) copystring(defs[*i].name, name); });
    ICOMMAND(splitcount, "", (), intret(defs.length()));
    ICOMMAND(splitfield, "is", (int *i, char *field),
    {
        if(!defs.inrange(*i)) { result(""); return; }
        const splitdef &d = defs[*i];
        if(!strcmp(field, "name")) result(d.name);
        else if(!strcmp(field, "type")) result(splittypes[d.type]);
        else if(!strcmp(field, "desc"))
        {
            string buf;
            switch(d.type)
            {
                case SPLIT_ZONE: formatstring(buf, "zone %.0f %.0f %.0f - %.0f %.0f %.0f", d.lo.x, d.lo.y, d.lo.z, d.hi.x, d.hi.y, d.hi.z); break;
                case SPLIT_PICKUP: formatstring(buf, "pickup of entity %d", d.ent); break;
                case SPLIT_TELEPORT: formatstring(buf, "teleport %d", d.ent); break;
                case SPLIT_WHEN: formatstring(buf, "when [%s]", d.expr); break;
                default: copystring(buf, "called by the map script"); break;
            }
            result(buf);
        }
        else result("");
    });

    static int runtimerticksnow() { bool running; return runtimerticks(running); }

    static bool hasfired(int def)
    {
        loopv(hits) if(hits[i].def == def) return true;
        return false;
    }

    static void fire(int def)
    {
        bool running;
        int t = runtimerticks(running);
        if(!running || !defs.inrange(def) || hasfired(def)) return;
        splithit &h = hits.add();
        h.def = def;
        h.tick = t;
        h.millis = totalmillis;
    }

    // feet position at the end of the previous tick, so a zone thinner than one tick's movement
    // is not stepped over; a longer jump than MAXSTEP (teleport, respawn) is not swept
    static vec lastfeet(0, 0, 0);
    static bool havelastfeet = false;
    static const float MAXSTEP = 64;

    // prints the entity index of every item player1 picks up and every teleport it uses
    MOD(VARP, splitlog, 0, 0, 1);

    void splitentityused(int n, fpsent *d, bool teleport)
    {
        if(d != player1) return;
        if(teleport) havelastfeet = false;
        if(splitlog)
        {
            const vector<extentity *> &ents = entities::getents();
            if(ents.inrange(n)) conoutf("\f4split log: %s entity %d (%s) at %.3f s", teleport ? "teleport" : "picked up", n, entities::entname(ents[n]->type), runtimerticksnow()*SIMTICK/1000.0f);
        }
        loopv(defs) if(defs[i].ent == n && defs[i].type == (teleport ? SPLIT_TELEPORT : SPLIT_PICKUP)) fire(i);
    }

    // called by map scripts
    ICOMMAND(splithere, "s", (char *name),
    {
        int i = finddef(name);
        if(i < 0) { adddef(name, SPLIT_SCRIPT); i = defs.length()-1; }
        fire(i);
    });

    // helpers for splitwhen conditions
    extern int numkilled;
    ICOMMAND(splitkills, "", (), intret(numkilled));
    ICOMMAND(splittriggered, "i", (int *tag),
    {
        const vector<extentity *> &ents = entities::getents();
        loopv(ents) if(ents[i]->attr4 == *tag && ents[i]->type == MAPMODEL && ((fpsentity *)ents[i])->triggerstate == TRIGGERED) { intret(1); return; }
        intret(0);
    });

    void splitresults(vector<splittime> &out)
    {
        out.setsize(0);
        loopv(hits) if(defs.inrange(hits[i].def))
        {
            splittime &t = out.add();
            copystring(t.name, defs[hits[i].def].name);
            t.tick = hits[i].tick;
        }
    }

    // what the HUD compares against, refreshed when the run's category or the statistics change
    static vector<splittime> pbsplits;
    static vector<splitsegment> bestsegs;
    static string comparekey = "";
    static int pbms = -1;

    static void refreshcomparison(const char *map, const char *kind, int mode)
    {
        bool segmented = inputsplicecount() > 0;
        defformatstring(key, "%s %s %d %d %d", map, kind, mode, segmented ? 1 : 0, statsversion());
        if(!strcmp(key, comparekey)) return;
        // a finished run stays compared with the PB it was run against, not with itself
        if(runtimerfinished() && comparekey[0]) return;
        copystring(comparekey, key);
        statssplitcompare(map, kind, mode, segmented, pbsplits, bestsegs, pbms);
    }

    static bool insidebox(const vec &p, const vec &lo, const vec &hi)
    {
        return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && p.z >= lo.z && p.z <= hi.z;
    }

    // whether the segment a-b touches the box (slab test)
    // entry: where along a-b the box is first touched, 0..1
    static bool segmenthitsbox(const vec &a, const vec &b, const vec &lo, const vec &hi, float *entry = NULL)
    {
        float t0 = 0, t1 = 1;
        loopi(3)
        {
            float d = b[i] - a[i];
            if(fabs(d) < 1e-6f) { if(a[i] < lo[i] || a[i] > hi[i]) return false; continue; }
            float n = (lo[i] - a[i]) / d, f = (hi[i] - a[i]) / d;
            if(n > f) swap(n, f);
            t0 = max(t0, n);
            t1 = min(t1, f);
            if(t0 > t1) return false;
        }
        if(entry) *entry = t0;
        return true;
    }

    // the chosen ghost replay's splits: recorded ones for a run of this player, else the zones
    // rebuilt from its ghost path; with its finish
    static vector<splittime> ghostsplits;
    static bool ghostcompare = false;
    static string ghostcomparekey = "";

    static void zonesfrompath(const vector<ghostsample> &samples, const vector<int> &maps)
    {
        vec eye(0, 0, player1->eyeheight);
        loopv(defs) if(defs[i].type == SPLIT_ZONE)
        {
            const splitdef &d = defs[i];
            int tick = -1;
            loopvj(samples)
            {
                vec b = vec(samples[j].o).sub(eye);
                if(j && maps[j-1] == maps[j])
                {
                    vec a = vec(samples[j-1].o).sub(eye);
                    float entry;
                    if(segmenthitsbox(a, b, d.lo, d.hi, &entry)) { tick = int(ceilf(samples[j-1].tick + entry*(samples[j].tick - samples[j-1].tick))); break; }
                }
                else if(insidebox(b, d.lo, d.hi)) { tick = samples[j].tick; break; }
            }
            if(tick < 0) continue;
            splittime &t = ghostsplits.add();
            copystring(t.name, d.name);
            t.tick = tick;
        }
    }

    static void refreshghostcomparison()
    {
        const char *file;
        const vector<ghostsample> *samples;
        const vector<int> *maps;
        int runtime;
        if(!ghostchosenreplay(file, samples, maps, runtime)) { ghostcompare = false; ghostcomparekey[0] = '\0'; return; }
        defformatstring(key, "%s|%s|%d", file, defskey, statsversion());
        if(ghostcompare && !strcmp(key, ghostcomparekey)) return;
        copystring(ghostcomparekey, key);
        ghostcompare = true;
        if(statssplitsofreplay(file, ghostsplits)) return;
        ghostsplits.setsize(0);
        zonesfrompath(*samples, *maps);
        if(runtime >= 0)
        {
            splittime &f = ghostsplits.add();
            copystring(f.name, "finish");
            f.tick = runtime / SIMTICK;
        }
    }

    static int ghosttick(const char *name)
    {
        if(!ghostcompare) return -1;
        loopv(ghostsplits) if(!strcmp(ghostsplits[i].name, name)) return ghostsplits[i].tick;
        return -1;
    }

    // How the finished run compares: a new PB (or the first finish), faster than the chosen ghost
    // replay without being a PB, or slower than both
    int splitsfinishresult()
    {
        if(!runtimerfinished()) return FINISH_NONE;
        bool running;
        int t = runtimerticks(running);
        if(pbms < 0 || t < pbms/SIMTICK) return FINISH_PB;
        const char *file;
        const vector<ghostsample> *s;
        const vector<int> *maps;
        int ghostms;
        if(ghostchosenreplay(file, s, maps, ghostms) && ghostms >= 0 && t < ghostms/SIMTICK) return FINISH_FASTER;
        return FINISH_SLOWER;
    }
    ICOMMAND(runfinishresult, "", (), intret(splitsfinishresult()));

    void splittick()
    {
        const inputattempt *first = inputrunfirst();
        const char *kind = runtimerkindname(), *map = first ? first->map : "";
        bool editing = editmode || !kind;
        if(editing)
        {
            kind = editkinds[clamp(spliteditkind, 0, 2)];
            map = getclientmap();
        }
        if(!map[0]) { defs.setsize(0); hits.setsize(0); defskey[0] = '\0'; splitmap[0] = '\0'; return; }
        defformatstring(key, "%s %s", map, kind);
        if(strcmp(key, defskey)) { copystring(defskey, key); loaddefs(map, kind); hits.setsize(0); }
        if(editing) return;
        refreshcomparison(first->map, kind, first->mode);
        refreshghostcomparison();

        bool running;
        int t = runtimerticks(running);
        // a state load or a new run moves the clock back
        if(t < lasttick)
        {
            havelastfeet = false;
            if(t <= 1) hits.setsize(0);
            else while(hits.length() && hits.last().tick > t) hits.pop();
        }
        lasttick = t;
        if(!running || player1->state != CS_ALIVE) { havelastfeet = false; return; }
        vec feet = player1->feetpos();
        bool sweep = havelastfeet && lastfeet.dist(feet) <= MAXSTEP;
        loopv(defs) if(!hasfired(i))
        {
            const splitdef &d = defs[i];
            if(d.type == SPLIT_ZONE)
            {
                if(sweep ? segmenthitsbox(lastfeet, feet, d.lo, d.hi) : insidebox(feet, d.lo, d.hi)) fire(i);
            }
            else if(d.type == SPLIT_WHEN && d.expr[0] && execute(d.expr)) fire(i);
        }
        lastfeet = feet;
        havelastfeet = true;
    }

    // zones as wire boxes, in edit mode or with splitshowzones
    void rendersplitzones()
    {
        if(!editmode && !splitshowzones) return;
        loopv(defs) if(defs[i].type == SPLIT_ZONE)
        {
            const splitdef &d = defs[i];
            int color = hasfired(i) ? 0x40FF80 : 0xFFC040;
            vec c[8];
            loopj(8) c[j] = vec(j&1 ? d.hi.x : d.lo.x, j&2 ? d.hi.y : d.lo.y, j&4 ? d.hi.z : d.lo.z);
            static const int edges[12][2] = { {0,1}, {2,3}, {4,5}, {6,7}, {0,2}, {1,3}, {4,6}, {5,7}, {0,4}, {1,5}, {2,6}, {3,7} };
            loopj(12) particle_flare(c[edges[j][0]], c[edges[j][1]], 1, PART_STREAK, color, 0.5f);
            particle_textcopy(vec(d.lo).add(d.hi).mul(0.5f), d.name, PART_TEXT, 1, color, 3.0f);
        }
    }

    static int pbtick(const char *name)
    {
        loopv(pbsplits) if(!strcmp(pbsplits[i].name, name)) return pbsplits[i].tick;
        return -1;
    }

    static int bestsegment(const char *prev, const char *name)
    {
        loopv(bestsegs) if(!strcmp(bestsegs[i].prev, prev) && !strcmp(bestsegs[i].name, name)) return bestsegs[i].ticks;
        return -1;
    }

    // sum of the best segments along the PB's order of splits, -1 if one is missing
    static int sumofbest()
    {
        int sum = 0;
        loopv(pbsplits)
        {
            int b = bestsegment(i ? pbsplits[i-1].name : "", pbsplits[i].name);
            if(b < 0) return -1;
            sum += b;
        }
        return pbsplits.length() ? sum : -1;
    }

    static void formattime(char *dst, int ticks)
    {
        int ms = ticks*SIMTICK;
        nformatstring(dst, MAXSTRLEN, "%d:%02d.%03d", ms/60000, (ms/1000)%60, ms%1000);
    }

    static void formatdelta(char *dst, int ticks)
    {
        int ms = abs(ticks)*SIMTICK;
        nformatstring(dst, MAXSTRLEN, "%s%s%d.%03d", ticks > 0 ? "\f3+" : ticks < 0 ? "\f0-" : "\f7", "", ms/1000, ms%1000);
    }

    // one HUD line for hit i: name, time (yellow for a new best segment), delta to the PB's
    // split, delta to the best time of the segment
    static void hitline(int i, char *dst)
    {
        const splithit &h = hits[i];
        const char *name = defs[h.def].name, *prev = i ? defs[hits[i-1].def].name : "";
        string t, d1 = "", d2 = "";
        formattime(t, h.tick);
        int pb = pbtick(name), gt = ghosttick(name);
        if(pb >= 0) { string d; formatdelta(d, h.tick - pb); formatstring(d1, "  \f7pb %s", d); }
        if(gt >= 0) { string d; formatdelta(d, h.tick - gt); concformatstring(d1, "  \f7%s %s", ghostname(), d); }
        int best = bestsegment(prev, name), seg = h.tick - (i ? hits[i-1].tick : 0);
        if(best >= 0) { string d; formatdelta(d, seg - best); formatstring(d2, "  \f7best %s", d); }
        nformatstring(dst, MAXSTRLEN, "\f7%s %s%s%s%s", name, best >= 0 && seg < best ? "\f2" : "", t, d1, d2);
    }

    extern float runtimerscale, runtimerx, runtimery;
    extern bool ghostdelta(int &delta);
    extern const char *ghostname();

    // the split HUD's lines and their alpha
    static void hudlines(vector<char *> &lines, vector<int> &alphas)
    {
        if(!runtimerkindname()) return;
        int alpha = 255;
        if(splitfade && hits.length())
        {
            int age = totalmillis - hits.last().millis - splitfade;
            if(age > 0) alpha = int(255 * clamp(1 - age / float(SPLITFADEMS), 0.0f, 1.0f));
        }
        if(!splithud || !alpha) {}
        else if(splitlist)
        {
            loopv(hits) if(defs.inrange(hits[i].def)) { string l; hitline(i, l); lines.add(newstring(l)); }
            int sob = sumofbest();
            if(sob >= 0) { string s, l; formattime(s, sob); formatstring(l, "\f4sum of best %s", s); lines.add(newstring(l)); }
        }
        else if(hits.length() && defs.inrange(hits.last().def))
        {
            string l;
            hitline(hits.length()-1, l);
            lines.add(newstring(l));
        }
        while(alphas.length() < lines.length()) alphas.add(alpha);
        bool running;
        int t = runtimerticks(running);
        int gt = ghosttick("finish");
        if(splithud && runtimerfinished() && (pbms >= 0 || gt >= 0))
        {
            string ft, l;
            formattime(ft, t);
            formatstring(l, "\f7finish %s%s", pbms >= 0 && t < pbms/SIMTICK ? "\f2" : "", ft);
            if(pbms >= 0) { string d; formatdelta(d, t - pbms/SIMTICK); concformatstring(l, "  \f7pb %s", d); }
            if(gt >= 0) { string g; formatdelta(g, t - gt); concformatstring(l, "  \f7%s %s", ghostname(), g); }
            lines.insert(0, newstring(l));
            alphas.insert(0, 255);
        }
        int delta;
        if(ghostdelta(delta)) { string d, l; formatdelta(d, delta); formatstring(l, "\f4%s %s", ghostname(), d); lines.add(newstring(l)); alphas.add(255); }
    }

    void drawsplits(int w, int h)
    {
        vector<char *> lines;
        vector<int> alphas;
        hudlines(lines, alphas);
        if(lines.empty()) return;

        const float scale = runtimerscale * 0.45f;
        pushhudmatrix();
        hudmatrix.scale(scale, scale, 1);
        flushhudmatrix();
        int zw = 0, zh = 0;
        text_bounds("0", zw, zh);
        float y = runtimery * h / scale + zh * runtimerscale / scale;
        loopv(lines)
        {
            int tw = 0, th = 0;
            text_bounds(lines[i], tw, th);
            draw_text(lines[i], int(runtimerx * w / scale - tw/2.0f), int(y), 255, 255, 255, alphas[i]);
            y += th;
        }
        pophudmatrix();
        lines.deletearrays();
    }

    // harness: the current run's splits as "name tick" pairs
    ICOMMAND(splittimes, "", (),
    {
        vector<char> buf;
        loopv(hits) if(defs.inrange(hits[i].def))
        {
            defformatstring(s, "%s%s %d", buf.length() ? " " : "", defs[hits[i].def].name, hits[i].tick);
            buf.put(s, strlen(s));
        }
        buf.add('\0');
        result(buf.getbuf());
    });
}

namespace game
{
    // harness: the split HUD's lines, joined with " | "
    ICOMMAND(splithudtext, "", (),
    {
        vector<char *> lines;
        vector<int> alphas;
        hudlines(lines, alphas);
        vector<char> buf;
        loopv(lines)
        {
            if(i) buf.put(" | ", 3);
            filtertext(lines[i], lines[i], false, false, strlen(lines[i]));
            buf.put(lines[i], strlen(lines[i]));
        }
        buf.add('\0');
        lines.deletearrays();
        result(buf.getbuf());
    });
}
