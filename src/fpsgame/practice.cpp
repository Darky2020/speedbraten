// Practice visualizers: what the game tests the player against, and the health of monsters and
// barrels, drawn outside edit mode. practicevisualizers (an assist, off at every launch) turns them on; the
// practice* toggles pick which. Flags, jumppads, teleports and level triggers act when the feet
// enter a sphere around them; a race finish cube is tested against the eye, so it is drawn one eye
// height lower, where the body has to be. A spawn box is the player's bounding box with the eye
// on the spawn point, where spawning puts it. The engine draws the clip, noclip, death and
// gameclip material (practiceclip and the like). Bounce edges are the top edges where rising
// alongside the side counts as touching a floor for a moment, coloured by what that gives after a
// standing jump from the floor in front.

#include "game.h"
#include "sbcompat.h"

namespace game
{
    MOD(VARP, practicejumppads, 0, 1, 1);
    MOD(VARP, practiceteleports, 0, 1, 1);
    MOD(VARP, practiceflags, 0, 1, 1);
    MOD(VARP, practiceracefinish, 0, 1, 1);
    MOD(VARP, practicecampaignfinish, 0, 1, 1); // the triggers that end or change a campaign map
    MOD(VARP, practicetriggers, 0, 1, 1);       // the other level triggers
    MOD(VARP, practicespawns, 0, 1, 1);
    MOD(VARP, practicegroups, 0, 1, 1);         // monsters by their group (tag)
    MOD(VARP, practicehealth, 0, 1, 1);         // damaged monsters
    MOD(VARP, practicebarrels, 0, 1, 1);        // barrels' health
    MOD(VARP, practicexray, 0, 0, 1);           // living monsters' boxes through walls
    MOD(VARP, practicexrayradius, 0, 1024, 100000); // how far away they show; 0: the whole map
    MOD(VARP, practicebounceedges, 0, 0, 1);    // top edges that clear built-up gravity when risen past
    MOD(VARP, practicebounceradius, 0, 256, 100000); // how far away they show; 0: the whole map
    MOD(VARP, practicebounceshallow, 0, 1, 1);  // the looked-at edge's approaches that slide up the side
    MOD(VARP, practicebounceheadon, 0, 1, 1);   // its head-on takeoff zones
    MOD(VARP, practicebouncespeed, 50, 165, 1000); // the horizontal speed approaches and zones are for
    MOD(VARP, practicelastjump, 0, 1, 1);       // where the last jump left the ground
    MOD(VARP, practicelastbump, 0, 1, 1);       // what the last bump did, on the HUD for a few seconds

    enum
    {
        COL_PADS = 0x40C0FF, COL_TELEPORT = 0xC060FF, COL_FLAG = 0xFFC040, COL_FINISH = 0x40FF80,
        COL_ARMED = 0xFF8040, COL_LOCKED = 0xC04040, COL_SPENT = 0x707070, COL_SPAWN = 0xE0E0E0
    };
    static const int groupcolours[] = { 0xFF4040, 0x40C0FF, 0xFFD040, 0x60FF60, 0xFF60FF, 0xFF9030, 0x40FFE0, 0xB080FF };
    static int groupcolour(int tag) { return groupcolours[(tag - 1) % (sizeof(groupcolours)/sizeof(groupcolours[0]))]; }

    static void color(int rgb)
    {
        gle::colorf(((rgb>>16)&0xFF)/255.0f, ((rgb>>8)&0xFF)/255.0f, (rgb&0xFF)/255.0f);
    }

    static void sphere(const vec &o, float r)
    {
        extentity e;
        e.o = o;
        renderentsphere(e, r);
    }

    static void ring(const vec &o, float r)
    {
        extentity e;
        e.o = o;
        renderentring(e, r, 0);
    }

    static void arrow(const vec &o, const vec &dir, float len)
    {
        extentity e;
        e.o = o;
        renderentarrow(e, dir, len);
    }

    // blue for a small gain through yellow to red at the most a standing jump gets; grey when a
    // standing jump doesn't reach the edge
    static int bouncecolour(float gain)
    {
        if(gain < 0) return 0x707070;
        float t = clamp((gain - 7)/(55 - 7), 0.0f, 1.0f);
        vec lo(0.25f, 0.5f, 1), mid(1, 0.9f, 0.25f), hi(1, 0.25f, 0.25f), c = t < 0.5f ? vec(lo).lerp(mid, t*2) : vec(mid).lerp(hi, t*2 - 1);
        return (int(c.x*255)<<16) | (int(c.y*255)<<8) | int(c.z*255);
    }

    static bool bounceinreach(const bounceedge &e)
    {
        return !practicebounceradius || camera1->o.dist(vec(e.a).add(e.b).mul(0.5f)) <= practicebounceradius;
    }

    // the edges near the camera that get a label and their approaches: one per 16 units
    static void nearbounceedges(vector<int> &out)
    {
        const vector<bounceedge> &edges = bounceedges(practicebouncespeed);
        vector<vec> placed;
        loopv(edges)
        {
            const bounceedge &e = edges[i];
            vec mid = vec(e.a).add(e.b).mul(0.5f);
            if(camera1->o.dist(mid) > 96) continue;
            bool crowded = false;
            loopvj(placed) if(placed[j].dist(mid) < 16) { crowded = true; break; }
            if(crowded) continue;
            placed.add(mid);
            out.add(i);
        }
    }

    // speed kept on an approach: green at 98% to red at 71%
    static const int approachcolours[NUMBOUNCEANGLES] = { 0x40FF60, 0xC0FF40, 0xFFB030, 0xFF4040 };

    // a frozen edge stays the looked-at one wherever the crosshair goes; it is kept by where it is, so
    // that a new search (at another speed) finds it again, and is let go when it can't be found
    static bool bouncefrozen = false;
    static vec bouncefrozenat(0, 0, 0);
    static int lookedframe = -1;

    // the edge under the crosshair: the one nearest the view direction within 8 degrees, at most 160
    // units away and not behind anything; -1 for none
    static int lookedbounceedge()
    {
        static int looked = -1;
        if(lookedframe == totalmillis) return looked;
        lookedframe = totalmillis;
        looked = -1;
        const vector<bounceedge> &edges = bounceedges(practicebouncespeed);
        if(bouncefrozen)
        {
            loopv(edges) if(vec(edges[i].a).add(edges[i].b).mul(0.5f).dist(bouncefrozenat) < 0.5f) { looked = i; break; }
            if(looked < 0) bouncefrozen = false;
            else return looked;
        }
        vec view;
        vecfromyawpitch(camera1->yaw, camera1->pitch, 1, 0, view);
        float best = cosf(8*RAD);
        loopv(edges)
        {
            const bounceedge &e = edges[i];
            vec to = vec(e.a).add(e.b).mul(0.5f).sub(camera1->o);
            float dist = to.magnitude();
            if(dist > 160 || dist < 1) continue;
            float c = to.dot(view)/dist;
            if(c <= best) continue;
            if(bounceray(camera1->o, vec(to).div(dist), dist) < dist - 2) continue;
            best = c;
            looked = i;
        }
        return looked;
    }

    static bool lastjumpshown()
    {
        static string map = "";
        if(strcmp(map, getclientmap())) { copystring(map, getclientmap()); lastjumpmillis = -1; }
        return practicelastjump && lastjumpmillis >= 0;
    }

    // the last bump's angle to the edge in degrees, 0 skimming along it, 90 straight at it: against the
    // wall first touched, or without one the nearest bounce edge; -1 when neither is known
    static float bumpangle()
    {
        vec n = lastbump.wall;
        if(n.iszero())
        {
            const vector<bounceedge> &edges = bounceedges(practicebouncespeed);
            float best = 8;
            loopv(edges)
            {
                float d = vec(edges[i].a).add(edges[i].b).mul(0.5f).dist(lastbump.o);
                if(d < best) { best = d; n = edges[i].out; }
            }
            if(n.iszero()) return -1;
        }
        n.z = 0;
        float v = lastbump.entryvel.magnitude(), len = n.magnitude();
        if(v < 1 || len < 1e-3f) return -1;
        return asinf(clamp(fabs(lastbump.entryvel.dot(n))/(v*len), 0.0f, 1.0f))/RAD;
    }

    // the last bump's numbers under the crosshair until the next one; dying or respawning fades
    // them out over a second
    void drawpracticehud(int w, int h)
    {
        static string map = "";
        static int life = -1, bump = -1, fadestart = -1;
        if(strcmp(map, getclientmap())) { copystring(map, getclientmap()); lastbump.millis = -1; }
        if(lastbump.millis != bump) { bump = lastbump.millis; fadestart = -1; life = player1->lifesequence; }
        if(fadestart < 0 && (player1->state == CS_DEAD || player1->lifesequence != life)) fadestart = lastmillis;
        if(!practicevisualizers || !practicelastbump || editmode || lastbump.millis < 0) return;
        if(lastmillis < lastbump.millis) return;
        int alpha = 255;
        if(fadestart >= 0)
        {
            int age = lastmillis - fadestart;
            if(age >= 1000 || age < 0) return;
            alpha = 255*(1000 - age)/1000;
        }
        static const char * const kinds[3] = { "landing bump", "stair bump", "stair bump + jump" };
        string lines[5];
        int n = 0;
        formatstring(lines[n++], "%s  %+d up", kinds[lastbump.kind], int(lastbump.upafter - lastbump.upbefore + (lastbump.upafter >= lastbump.upbefore ? 0.5f : -0.5f)));
        formatstring(lines[n++], "entry %d u/s, after %d (lost %d)", int(lastbump.entry + 0.5f), int(lastbump.after + 0.5f), int(max(lastbump.entry - lastbump.after, 0.0f) + 0.5f));
        float angle = lastbump.kind == BUMP_LANDING ? bumpangle() : -1;
        if(angle >= 0) formatstring(lines[n++], "entry angle %d deg to the edge%s", int(angle + 0.5f), lastbump.wall.iszero() ? "" : " (wall)");
        formatstring(lines[n++], "up %d -> %d u/s", int(lastbump.upbefore + (lastbump.upbefore >= 0 ? 0.5f : -0.5f)), int(lastbump.upafter + 0.5f));
        formatstring(lines[n++], "gravity cleared %d u/s", int(lastbump.gravity + 0.5f));
        const float scale = 0.4f;
        pushhudmatrix();
        hudmatrix.scale(scale, scale, 1);
        flushhudmatrix();
        float y = 0.6f*h/scale;
        loopi(n)
        {
            int tw = 0, th = 0;
            text_bounds(lines[i], tw, th);
            draw_text(lines[i], int(w/scale/2 - tw/2), int(y), 255, 255, 255, alpha);
            y += th;
        }
        pophudmatrix();
    }

    static void freezebounceedge()
    {
        if(bouncefrozen) bouncefrozen = false;
        else
        {
            lookedframe = -1;
            int looked = lookedbounceedge();
            if(looked < 0) return;
            const bounceedge &e = bounceedges(practicebouncespeed)[looked];
            bouncefrozenat = vec(e.a).add(e.b).mul(0.5f);
            bouncefrozen = true;
        }
        lookedframe = -1;
    }
    COMMANDN(practicebouncefreeze, freezebounceedge, "");
    ICOMMAND(practicebouncefrozen, "", (), intret(bouncefrozen ? 1 : 0));

    // the share of horizontal speed an approach loses sliding into the side
    static float approachloss(int angle) { return 1 - cosf(BOUNCEANGLES[angle]*RAD); }

    // jumping at the edge from the floor in front without touching the side, by takeoff distance at
    // practicebouncespeed (at any angle across the edge): up to zones[0] the body hits the wall, up to
    // zones[1] it steps up onto the top, up to zones[2] it lands on it while rising (the bump, a
    // narrow band), past that it flies over; false when a standing jump doesn't reach the top
    static bool headonzones(const bounceedge &e, vec &contact, float zones[3], float &window, float &reach)
    {
        float stair;
        if(!bounceflyover(e.height, reach, stair, window)) return false;
        float speed = practicebouncespeed;
        contact = vec(e.out).mul(4.2f).add(vec(e.a).add(e.b).mul(0.5f));
        contact.z -= e.height;
        zones[0] = bouncerunup(speed, stair);
        zones[1] = bouncerunup(speed, reach - window/2);
        zones[2] = bouncerunup(speed, reach + window/2);
        return true;
    }

    // the takeoff speed at which a jump covers room in secs, by bisection (bouncerunup grows with it)
    static float fitspeed(float room, float secs)
    {
        float lo = 0, hi = 1000;
        loopi(30) { float mid = (lo + hi)/2; if(bouncerunup(mid, secs) > room) hi = mid; else lo = mid; }
        return lo;
    }

    // the directions the zones are drawn in, -75 to 75 degrees from straight out, with how far the
    // floor in front is clear in each and whether the bump band there is on a floor
    enum { HEADONDIRS = 16 };
    struct headondir { vec dir; float room; bool band; };

    static void headondirs(const bounceedge &e, const vec &contact, const float zones[3], headondir dirs[HEADONDIRS])
    {
        vec side = vec(e.out.y, -e.out.x, 0);
        loopi(HEADONDIRS)
        {
            float a = (-75 + 150.0f*i/(HEADONDIRS - 1))*RAD;
            headondir &d = dirs[i];
            d.dir = vec(e.out).mul(cosf(a)).add(vec(side).mul(sinf(a)));
            d.room = bounceray(vec(contact).add(vec(0, 0, 1)), d.dir, 128);
            float at = (zones[1] + zones[2])/2;
            d.band = false;
            if(at <= d.room)
            {
                vec band = vec(d.dir).mul(at).add(contact);
                float ground = bounceray(vec(band).add(vec(0, 0, 1)), vec(0, 0, -1), 5);
                d.band = ground >= 0.5f && ground <= 3.5f;
            }
        }
    }

    // harness: the head-on zones of the bounce edges within r of x y z and in how many directions the
    // bump band is on a clear floor
    static void headonlist(float *x, float *y, float *z, float *r)
    {
        vector<char> buf;
        const vector<bounceedge> &edges = bounceedges(practicebouncespeed);
        loopv(edges)
        {
            const bounceedge &e = edges[i];
            if(vec(*x, *y, *z).dist(vec(e.a).add(e.b).mul(0.5f)) > *r) continue;
            vec contact;
            float zones[3], window, reach;
            if(!headonzones(e, contact, zones, window, reach)) continue;
            headondir dirs[HEADONDIRS];
            headondirs(e, contact, zones, dirs);
            int open = 0;
            float room = 0;
            loopj(HEADONDIRS) { if(dirs[j].band) open++; room = max(room, dirs[j].room); }
            defformatstring(line, "%sout %.0f %.0f zones %.1f %.1f %.1f window %.4f open %d of %d room %.1f", buf.length() ? "; " : "", e.out.x, e.out.y, zones[0], zones[1], zones[2], window, open, int(HEADONDIRS), room);
            buf.put(line, strlen(line));
        }
        buf.add('\0');
        result(buf.getbuf());
    }
    COMMANDN(practiceheadonlist, headonlist, "ffff");

    // an arc at radius r around the contact point, broken where the floor in front isn't clear that far
    static void headonarc(const vec &contact, const headondir dirs[HEADONDIRS], float r, float lift, bool bandonly)
    {
        gle::defvertex();
        gle::begin(GL_LINES);
        loopi(HEADONDIRS - 1)
        {
            const headondir &d0 = dirs[i], &d1 = dirs[i+1];
            if(r > d0.room || r > d1.room || (bandonly && (!d0.band || !d1.band))) continue;
            vec p0 = vec(d0.dir).mul(r).add(contact), p1 = vec(d1.dir).mul(r).add(contact);
            p0.z += lift;
            p1.z += lift;
            gle::attrib(p0);
            gle::attrib(p1);
        }
        gle::end();
    }

    static void drawheadon(const bounceedge &e)
    {
        vec contact;
        float zones[3], window, reach;
        if(!headonzones(e, contact, zones, window, reach)) return;
        headondir dirs[HEADONDIRS];
        headondirs(e, contact, zones, dirs);
        color(0xFF4040);
        headonarc(contact, dirs, zones[0], 0.2f, false);
        color(0xFFB030);
        headonarc(contact, dirs, zones[1] - 0.3f, 0.2f, false);
        // the bump band in three arcs so that it shows however narrow it is
        color(0x40FF60);
        loopk(3) headonarc(contact, dirs, zones[1] + (zones[2] - zones[1])*k/2, 0.2f + 0.1f*k, true);
    }

    static void drawbounceapproaches()
    {
        const vector<bounceedge> &edges = bounceedges(practicebouncespeed);
        int looked = lookedbounceedge();
        if(looked >= 0)
        {
            const bounceedge &e = edges[looked];
            if(practicebounceheadon) drawheadon(e);
            color(0xFFFFFF);
            gle::defvertex();
            gle::begin(GL_LINES);
            gle::attrib(vec(e.a).add(vec(0, 0, 0.3f)));
            gle::attrib(vec(e.b).add(vec(0, 0, 0.3f)));
            gle::end();
            if(practicebounceshallow) loopk(NUMBOUNCEANGLES) loopj(2) if(e.approaches & (1<<(2*k + j)))
            {
                vec takeoff, contact;
                bounceapproachpath(e, k, j, takeoff, contact);
                takeoff.z += 0.2f;
                contact.z = takeoff.z;
                color(approachcolours[k]);
                gle::defvertex();
                gle::begin(GL_LINES);
                gle::attrib(takeoff);
                gle::attrib(contact);
                gle::end();
                ring(takeoff, 1.5f);
            }
        }
    }

    static void drawbounceedges()
    {
        const vector<bounceedge> &edges = bounceedges(practicebouncespeed);
        gle::defvertex();
        loopv(edges)
        {
            const bounceedge &e = edges[i];
            if(!bounceinreach(e)) continue;
            int c = bouncecolour(bouncegain(e.height));
            // edges only a steep approach reaches cost most of the speed: drawn darker
            if(!e.keep && bouncegain(e.height) >= 0) c = ((c>>1)&0x7F7F7F);
            color(c);
            gle::begin(GL_LINES);
            gle::attrib(vec(e.a).add(vec(0, 0, 0.1f)));
            gle::attrib(vec(e.b).add(vec(0, 0, 0.1f)));
            gle::end();
        }
    }

    static void bounceedgesummary()
    {
        const vector<bounceedge> &edges = bounceedges(practicebouncespeed);
        int high = 0, steep = 0, kept[4] = { 0, 0, 0, 0 };
        loopv(edges)
        {
            const bounceedge &e = edges[i];
            if(bouncegain(e.height) < 0) high++;
            else if(!e.keep) steep++;
            else kept[e.keep > 0.97f ? 0 : e.keep > 0.9f ? 1 : e.keep > 0.8f ? 2 : 3]++;
        }
        defformatstring(sum, "%d edges: keep 98%% %d, 94%% %d, 87%% %d, 71%% %d, steep only %d, too high %d", edges.length(), kept[0], kept[1], kept[2], kept[3], steep, high);
        result(sum);
    }

    // harness: "height gain keep ax ay az bx by bz" of the bounce edges within r of x y z; with r < 0
    // how many edges keep which share of speed
    ICOMMAND(practicebounceedgelist, "ffff", (float *x, float *y, float *z, float *r),
    {
        vector<char> buf;
        const vector<bounceedge> &edges = bounceedges(practicebouncespeed);
        if(*r < 0) { bounceedgesummary(); return; }
        vec at(*x, *y, *z);
        loopv(edges)
        {
            const bounceedge &e = edges[i];
            if(at.dist(vec(e.a).add(e.b).mul(0.5f)) > *r) continue;
            defformatstring(line, "%s%.1f %.0f %.2f %.1f %.1f %.1f %.1f %.1f %.1f", buf.length() ? "; " : "", e.height, bouncegain(e.height), e.keep, e.a.x, e.a.y, e.a.z, e.b.x, e.b.y, e.b.z);
            buf.put(line, strlen(line));
        }
        defformatstring(total, "%s%d edges on the map", buf.length() ? "; " : "", edges.length());
        buf.put(total, strlen(total));
        buf.add('\0');
        result(buf.getbuf());
    });

    static void box(const vec &lo, const vec &hi)
    {
        vec c[8];
        loopi(8) c[i] = vec(i&1 ? hi.x : lo.x, i&2 ? hi.y : lo.y, i&4 ? hi.z : lo.z);
        static const int edges[12][2] = { {0,1}, {2,3}, {4,5}, {6,7}, {0,2}, {1,3}, {4,6}, {5,7}, {0,4}, {1,5}, {2,6}, {3,7} };
        gle::defvertex();
        gle::begin(GL_LINES);
        loopi(12) { gle::attrib(c[edges[i][0]]); gle::attrib(c[edges[i][1]]); }
        gle::end();
    }

    // whether a script ends or changes the map: a word of it is one of the commands that do
    static bool scriptexits(const char *script)
    {
        static const char * const exits[] = { "endsp", "sp", "map", "changemap", "dmsp" };
        for(const char *s = script; *s;)
        {
            while(*s && !isalnum(uchar(*s)) && *s != '_') s++;
            const char *w = s;
            while(isalnum(uchar(*s)) || *s == '_') s++;
            loopi(sizeof(exits)/sizeof(exits[0])) if(int(strlen(exits[i])) == s - w && !strncmp(w, exits[i], s - w)) return true;
        }
        return false;
    }

    static bool triggerexits(const extentity &e, bool endsp)
    {
        if(endsp) return true;
        if(!e.attr4) return false;
        defformatstring(name, "level_trigger_%d", e.attr4);
        const char *script = getalias(name);
        return script && scriptexits(script);
    }

    static void leveltriggers()
    {
        const vector<extentity *> &ents = entities::getents();
        loopv(ents)
        {
            const fpsentity &e = *(const fpsentity *)ents[i];
            float reach;
            bool locked, endsp;
            if(!entities::leveltrigger(e, reach, locked, endsp)) continue;
            bool exits = triggerexits(e, endsp);
            if(exits ? !practicecampaignfinish : !practicetriggers) continue;
            bool spent = e.triggerstate == TRIGGERED || e.triggerstate == TRIGGER_DISAPPEARED;
            color(exits ? COL_FINISH : spent ? COL_SPENT : locked ? COL_LOCKED : COL_ARMED);
            sphere(e.o, reach + player1->radius);
        }
    }

    static void monstergroups()
    {
        loopi(nummonsterslots())
        {
            int tag, maxhealth;
            fpsent *m = monsterslot(i, tag, maxhealth);
            if(!tag || m->state != CS_ALIVE) continue;
            color(groupcolour(tag));
            ring(m->feetpos(0.5f), m->radius + 2);
        }
    }

    // the boxes of living monsters within practicexrayradius, through walls: monsters stuck where
    // nothing can reach them still show
    static void monsterxray()
    {
        glDisable(GL_DEPTH_TEST);
        loopi(nummonsterslots())
        {
            int tag, maxhealth;
            fpsent *m = monsterslot(i, tag, maxhealth);
            if(m->state != CS_ALIVE || (practicexrayradius && m->o.dist(camera1->o) > practicexrayradius)) continue;
            color(tag ? groupcolour(tag) : 0xFF5050);
            box(vec(m->o).sub(vec(m->radius, m->radius, m->eyeheight)), vec(m->o).add(vec(m->radius, m->radius, m->aboveeye)));
        }
        glEnable(GL_DEPTH_TEST);
    }

    void renderpractice()
    {
        if(!practicevisualizers || editmode) return;
        const vector<extentity *> &ents = entities::getents();
        loopv(ents)
        {
            const extentity &e = *ents[i];
            float r;
            switch(e.type)
            {
                case JUMPPAD:
                    if(!practicejumppads) break;
                    color(COL_PADS);
                    sphere(e.o, 12);
                    arrow(e.o, vec((int)(char)e.attr3*10.0f, (int)(char)e.attr2*10.0f, e.attr1*12.5f).normalize(), 24);
                    break;
                case TELEPORT:
                    if(!practiceteleports) break;
                    color(COL_TELEPORT);
                    sphere(e.o, 16);
                    break;
                case TELEDEST:
                {
                    if(!practiceteleports) break;
                    color(COL_TELEPORT);
                    vec dir;
                    vecfromyawpitch(e.attr1, 0, 1, 0, dir);
                    arrow(e.o, dir, 16);
                    break;
                }
                case PLAYERSTART:
                case RESPAWNPOINT:
                {
                    if(!practicespawns || (e.type == PLAYERSTART && e.attr2 < 0)) break;
                    const fpsent &p = *player1;
                    color(COL_SPAWN);
                    box(vec(e.o).sub(vec(p.radius, p.radius, p.eyeheight)), vec(e.o).add(vec(p.radius, p.radius, p.aboveeye)));
                    vec dir;
                    vecfromyawpitch(e.attr1, 0, 1, 0, dir);
                    arrow(e.o, dir, 8);
                    break;
                }
            }
            if(practiceracefinish && racerules() && racegoal(e, r))
            {
                color(COL_FINISH);
                vec c(e.o);
                c.z -= player1->eyeheight;
                box(vec(c).sub(r), vec(c).add(r));
            }
        }
        if(practiceflags)
        {
            vector<vec> flags;
            float fr = flagtouchlocs(flags);
            color(COL_FLAG);
            loopv(flags) sphere(flags[i], fr);
        }
        if(practicebounceedges) { drawbounceedges(); drawbounceapproaches(); }
        if(lastjumpshown())
        {
            color(0x60E0FF);
            ring(vec(lastjumpat).add(vec(0, 0, 0.2f)), 1.5f);
            gle::defvertex();
            gle::begin(GL_LINES);
            gle::attrib(vec(lastjumpat).add(vec(-0.6f, 0, 0.2f))); gle::attrib(vec(lastjumpat).add(vec(0.6f, 0, 0.2f)));
            gle::attrib(vec(lastjumpat).add(vec(0, -0.6f, 0.2f))); gle::attrib(vec(lastjumpat).add(vec(0, 0.6f, 0.2f)));
            gle::end();
        }
        if(m_sp)
        {
            leveltriggers();
            if(practicegroups) monstergroups();
            if(practicexray) monsterxray();
        }
    }

    // a health bar with the number above it; returns how high the two reach above o
    static float healthlabel(const vec &o, int health, int maxhealth)
    {
        float fill = clamp(float(health)/maxhealth, 0.0f, 1.0f);
        int c = fill <= 0.25f ? 0xFF4040 : fill <= 0.5f ? 0xFF8000 : 0x40FF40;
        particle_meter(vec(o).add(vec(0, 0, 1)), fill, PART_METER, 1, c, 0, 2.0f);
        defformatstring(num, "%d", health);
        particle_textcopy(vec(o).add(vec(0, 0, 3.5f)), num, PART_TEXT, 1, 0xFFFFFF, 2.0f);
        return 6;
    }

    // monsters' group numbers and health, barrels' health, as particles of the frame being drawn
    void renderpracticelabels()
    {
        static int lastframe = -1;
        if(!practicevisualizers || editmode || lastframe == totalmillis || !canaddparticles()) return;
        lastframe = totalmillis;
        if(practicegroups || practicehealth) loopi(nummonsterslots())
        {
            int tag, maxhealth;
            fpsent *m = monsterslot(i, tag, maxhealth);
            if(m->state != CS_ALIVE) continue;
            vec above = m->abovehead();
            float up = 0;
            if(practicehealth && m->health < maxhealth && maxhealth > 0) up = healthlabel(above, m->health, maxhealth);
            if(practicegroups && tag)
            {
                defformatstring(label, "%d", tag);
                particle_textcopy(vec(above).add(vec(0, 0, up + 3.5f)), label, PART_TEXT, 1, groupcolour(tag), 2.5f);
            }
        }
        if(lastjumpshown())
        {
            // how far the takeoff was from the head-on bump band of the edge being looked at
            string label;
            formatstring(label, "jump %d u/s", int(lastjumpspeed + 0.5f));
            int looked = practicebounceedges ? lookedbounceedge() : -1;
            vec contact;
            float zones[3], window, reach;
            if(looked >= 0 && headonzones(bounceedges(practicebouncespeed)[looked], contact, zones, window, reach))
            {
                float r = vec(lastjumpat.x - contact.x, lastjumpat.y - contact.y, 0).magnitude();
                // seen from the run up: closer to the wall than the band is a late jump
                if(r < zones[1]) concformatstring(label, ", %.1f late", zones[1] - r);
                else if(r > zones[2]) concformatstring(label, ", %.1f early", r - zones[2]);
                else concatstring(label, ", on the band");
            }
            particle_textcopy(vec(lastjumpat).add(vec(0, 0, 2)), label, PART_TEXT, 1, 0x60E0FF, 1.0f);
        }
        if(practicebounceedges && lookedbounceedge() >= 0)
        {
            const bounceedge &e = bounceedges(practicebouncespeed)[lookedbounceedge()];
            vec contact;
            float zones[3], window, reach;
            if(practicebounceheadon && headonzones(e, contact, zones, window, reach))
            {
                headondir dirs[HEADONDIRS];
                headondirs(e, contact, zones, dirs);
                int best = -1;
                float room = 0;
                loopj(HEADONDIRS)
                {
                    if(dirs[j].band && (best < 0 || abs(j - HEADONDIRS/2) < abs(best - HEADONDIRS/2))) best = j;
                    room = max(room, dirs[j].room);
                }
                string label;
                vec at;
                if(best >= 0)
                {
                    formatstring(label, "fly over: 0%%, %.1f ms at %d u/s", window*1000, int(practicebouncespeed + 0.5f));
                    at = vec(dirs[best].dir).mul((zones[1] + zones[2])/2).add(contact);
                }
                else
                {
                    // the band lies past the clear floor in every direction, or there is room but no
                    // floor where the band is
                    float bandat = (zones[1] + zones[2])/2;
                    if(room < bandat) formatstring(label, "fly over: 0%%, %.1f ms, needs <= %d u/s here", window*1000, int(fitspeed(room, reach)));
                    else copystring(label, "fly over: no floor to jump from");
                    at = vec(e.out).mul(min(bandat, dirs[HEADONDIRS/2].room - 1)).add(contact);
                }
                if(best >= 0 || fitspeed(room, reach) >= 50) particle_textcopy(vec(at).add(vec(0, 0, 3)), label, PART_TEXT, 1, 0x40FF60, 1.5f);
                // the red and orange arcs, named where they are drawn nearest straight out
                static const char * const names[2] = { "hits wall", "steps up" };
                static const int colours[2] = { 0xFF4040, 0xFFB030 };
                loopk(2)
                {
                    float r = k ? zones[1] - 0.3f : zones[0];
                    int nearest = -1;
                    loopj(HEADONDIRS) if(r <= dirs[j].room && (nearest < 0 || abs(j - HEADONDIRS/2) < abs(nearest - HEADONDIRS/2))) nearest = j;
                    if(nearest >= 0) particle_textcopy(vec(dirs[nearest].dir).mul(r).add(contact).add(vec(0, 0, 2)), names[k], PART_TEXT, 1, colours[k], 1.5f);
                }
            }
            if(practicebounceshallow) loopk(NUMBOUNCEANGLES) loopj(2) if(e.approaches & (1<<(2*k + j)))
            {
                vec takeoff, contact;
                bounceapproachpath(e, k, j, takeoff, contact);
                defformatstring(loss, "-%d%%  -%d u/s", int(approachloss(k)*100 + 0.5f), int(approachloss(k)*practicebouncespeed + 0.5f));
                particle_textcopy(vec(takeoff).add(vec(0, 0, 3)), loss, PART_TEXT, 1, approachcolours[k], 1.5f);
            }
        }
        if(practicebounceedges)
        {
            // one label per stretch: none within 16 units of one already placed
            const vector<bounceedge> &edges = bounceedges(practicebouncespeed);
            vector<int> nearby;
            nearbounceedges(nearby);
            loopv(nearby)
            {
                const bounceedge &e = edges[nearby[i]];
                vec mid = vec(e.a).add(e.b).mul(0.5f);
                float gain = bouncegain(e.height);
                string label;
                if(gain < 0) copystring(label, "needs extra height");
                else if(e.keep) formatstring(label, "+%d, %d%%", int(gain + 0.5f), int(e.keep*100 + 0.5f));
                else formatstring(label, "+%d, steep", int(gain + 0.5f));
                particle_textcopy(vec(mid).add(vec(0, 0, 2)), label, PART_TEXT, 1, bouncecolour(gain), 1.5f);
            }
        }
        if(practicebarrels) loopi(nummovableslots())
        {
            int health, maxhealth;
            dynent *m = movableslot(i, health, maxhealth);
            if(m->state != CS_ALIVE || maxhealth <= 0) continue;
            healthlabel(vec(m->o).add(vec(0, 0, m->aboveeye + 1)), health, maxhealth);
        }
    }
}
