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
    MOD(VARP, practicestuckexits, 0, 1, 1);     // while stuck building fall speed: where letting go of the keys throws you

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

    // Collision tests fill the engine's dynent cache, which belongs to the simulation (states save it, replay
    // hashes check it): simulations for the visuals run inside one of these, which puts the cache back.
    struct keepdynentcache
    {
        vector<int> cache;
        bool saved;
        keepdynentcache() { saved = savedynentcache(cache); }
        ~keepdynentcache() { if(saved) loaddynentcache(cache); }
    };

    // the stuck finder's simulations: monsters and other movers come and go, so only the map holds a spot
    struct mapcollisionsonly
    {
        mapcollisionsonly() { collidedynents = false; }
        ~mapcollisionsonly() { collidedynents = true; }
    };

    // Stuck: wedged somewhere the game never sees a floor it can stand on (falling, or sliding on a steep
    // surface, where gravity runs along it and builds sideways speed too), so the player doesn't move while
    // gravity keeps adding to the fall speed. Letting go of the keys that hold it there finds the floor, and that tick's
    // remaining sub-steps keep the fall speed's length but point along whatever velocity is left: a throw
    // of a few long hops, each checked for collisions only where it ends. Each exit is simulated on a copy
    // of the player with the keys that could be held at the release.
    // A release a tick earlier or later can land a hop shorter or longer, so the landings of releases on
    // each of the next STUCKSPREAD ticks are shown too.
    enum { STUCKMINFALL = 800, STUCKPATHTICKS = 100, STUCKSPREADTICKS = 40, STUCKSETTLE = 20, NUMSTUCKKEYS = 8, STUCKSPREAD = 10, STUCKREFRESH = 500 };
    static const struct { int move, strafe; const char *name; } stuckkeys[NUMSTUCKKEYS] =
    {
        { 0, 0, "none" }, { 0, 1, "A" }, { 0, -1, "D" }, { -1, 0, "S" }, { -1, 1, "S+A" }, { -1, -1, "S+D" },
        { 1, 1, "W+A" }, { 1, -1, "W+D" } // adding a key to the held W: often enough to come loose
    };

    // wall: a hop of the throw crosses map geometry (mapmodels aren't in it); hold: holding W with these
    // keys keeps the player stuck, so they can be pressed before W is let go
    struct stuckexit { vector<vec> path; bool out, wall, hold; };
    static stuckexit stuckexits[NUMSTUCKKEYS][1];
    static vec stucklandings[NUMSTUCKKEYS][STUCKSPREAD];
    static bool stuckspreadout[NUMSTUCKKEYS][STUCKSPREAD];
    static vec stuckpos(0, 0, 0);
    static int stucksince = -1, stuckcomputed = -1;
    // the simulations are jobs run a few per frame within a time budget; a full round every STUCKREFRESH ms
    enum { STUCKJOBS = NUMSTUCKKEYS*(1 + STUCKSPREAD), STUCKBUDGETUS = 2000 };
    static int stuckcursor = 0;

    static bool isstuck()
    {
        if(!practicevisualizers || !practicestuckexits || editmode || player1->state != CS_ALIVE) { stucksince = -1; return false; }
        bool falls = player1->physstate == PHYS_FALL || player1->physstate == PHYS_SLIDE || player1->physstate == PHYS_SLOPE;
        if(!falls || player1->falling.magnitude() < STUCKMINFALL || player1->o.dist(stuckpos) > 0.01f)
        {
            stuckpos = player1->o;
            stucksince = -1;
            return false;
        }
        if(stucksince < 0) { stucksince = lastmillis; stuckcomputed = -1; stuckcursor = 0; loopi(NUMSTUCKKEYS) stuckexits[i][0].path.setsize(0); }
        return true;
    }

    // a copy of the player that keeps its keys for hold ticks, then lets go to keys k; false if it leaves the map
    static bool simulateexit(fpsent &sim, const physent &base, int hold, int k, int extrafall, vector<vec> *path, int ticks)
    {
        (physent &)sim = base;
        sim.jumping = false;
        sim.falling.z -= extrafall;
        loopi(hold) simulatetick(&sim, player1);
        sim.move = stuckkeys[k].move;
        sim.strafe = stuckkeys[k].strafe;
        if(path) path->add(sim.o);
        vec last = sim.o;
        int still = 0;
        loopi(ticks)
        {
            simulatetick(&sim, player1, path && i < 2 ? path : NULL);
            if(path && i >= 2) path->add(sim.o);
            if(sim.o.z < 0 || lookupmaterial(sim.o)&MAT_DEATH) return false;
            if(sim.physstate >= PHYS_SLOPE && vec(sim.vel).add(sim.falling).magnitude() < 50) break;
            // stuck again: nothing more will happen
            if(sim.o.dist(last) > 0.01f) { last = sim.o; still = 0; }
            else if(++still >= STUCKSETTLE) break;
        }
        return true;
    }

    // whether a hop of the throw crosses map geometry, the player's middle taken as the ray's start
    static bool throughwall(const vector<vec> &path)
    {
        vec mid(0, 0, player1->eyeheight - (player1->eyeheight + player1->aboveeye)/2);
        for(int i = 1; i < path.length(); i++)
        {
            vec a = vec(path[i-1]).sub(mid), ray = vec(path[i]).sub(path[i-1]);
            float len = ray.magnitude();
            if(len < 1e-3f) continue;
            ray.div(len);
            float d = raycube(a, ray, len, RAY_CLIPMAT);
            if(d > 0.01f && d < len - 0.01f) return true;
        }
        return false;
    }

    // holding W together with keys k for a while, the player stays where it is
    static bool holdsstuck(fpsent &sim, int k)
    {
        (physent &)sim = *player1;
        sim.jumping = false;
        sim.move = 1;
        sim.strafe = stuckkeys[k].strafe;
        loopi(40)
        {
            simulatetick(&sim, player1);
            if(sim.physstate >= PHYS_FLOOR || sim.o.dist(player1->o) > 0.01f) return false;
        }
        return true;
    }

    static void computestuckexits()
    {
        if(stuckcursor >= STUCKJOBS)
        {
            if(stuckcomputed >= 0 && lastmillis - stuckcomputed < STUCKREFRESH) return;
            stuckcursor = 0;
        }
        if(!stuckcursor) stuckcomputed = lastmillis;
        keepdynentcache keep;
        static fpsent sim;
        Uint64 start = SDL_GetPerformanceCounter(), budget = SDL_GetPerformanceFrequency()*STUCKBUDGETUS/1000000;
        while(stuckcursor < STUCKJOBS && SDL_GetPerformanceCounter() - start < budget)
        {
            int job = stuckcursor++, i = job/(1 + STUCKSPREAD), j = job%(1 + STUCKSPREAD);
            if(!j)
            {
                stuckexit &x = stuckexits[i][0];
                x.path.setsize(0);
                x.out = !simulateexit(sim, *player1, 0, i, 0, &x.path, STUCKPATHTICKS);
                x.wall = throughwall(x.path);
                x.hold = stuckkeys[i].move >= 0 && stuckkeys[i].strafe && holdsstuck(sim, i);
                // W with another key that keeps the player stuck is no exit
                if(stuckkeys[i].move > 0 && x.hold) x.path.setsize(0);
            }
            else
            {
                stuckspreadout[i][j-1] = !simulateexit(sim, *player1, j-1, i, 0, NULL, STUCKSPREADTICKS);
                stucklandings[i][j-1] = sim.o;
            }
        }
    }

    // the key sets whose throws land together (within STUCKMERGE units) share one marker, path and label
    enum { STUCKMERGE = 4 };
    struct stuckgroup { int first; string keys; float lo, hi; bool out, wall; };
    static vector<stuckgroup> stuckgroups;

    static void groupstuckexits()
    {
        stuckgroups.setsize(0);
        loopi(NUMSTUCKKEYS)
        {
            const stuckexit &x = stuckexits[i][0];
            if(x.path.length() < 2) continue;
            const vec &end = x.path[x.path.length()-1];
            float lo = 1e16f, hi = 0;
            bool out = x.out;
            loopj(STUCKSPREAD)
            {
                float d = stucklandings[i][j].dist(x.path[0]);
                lo = min(lo, d);
                hi = max(hi, d);
                out = out || stuckspreadout[i][j];
            }
            string keys;
            copystring(keys, stuckkeys[i].name);
            if(stuckkeys[i].strafe && !stuckkeys[i].move) concatstring(keys, x.hold ? " (press first)" : " (same moment)");
            stuckgroup *g = NULL;
            loopvj(stuckgroups)
            {
                const stuckexit &y = stuckexits[stuckgroups[j].first][0];
                if(y.path[y.path.length()-1].dist(end) <= STUCKMERGE) { g = &stuckgroups[j]; break; }
            }
            if(g)
            {
                concformatstring(g->keys, ", %s", keys);
                g->lo = min(g->lo, lo);
                g->hi = max(g->hi, hi);
                g->out = g->out || out;
                g->wall = g->wall || x.wall;
                continue;
            }
            g = &stuckgroups.add();
            g->first = i;
            copystring(g->keys, keys);
            g->lo = lo;
            g->hi = hi;
            g->out = out;
            g->wall = x.wall;
        }
    }

    static void drawstuckexits()
    {
        if(!isstuck()) return;
        computestuckexits();
        groupstuckexits();
        // the throws go through walls, so they show through them
        glDisable(GL_DEPTH_TEST);
        vec feet(0, 0, player1->eyeheight - 0.5f);
        // the throw of letting go of every key (key 0, always the first of its group) is drawn bold: parallel
        // strips, as GL lines are one pixel wide
        static const vec bold[5] = { vec(0, 0, 0), vec(0.35f, 0, 0), vec(-0.35f, 0, 0), vec(0, 0.35f, 0), vec(0, -0.35f, 0) };
        loopv(stuckgroups)
        {
            const stuckgroup &g = stuckgroups[i];
            const stuckexit &x = stuckexits[g.first][0];
            int strips = g.first == 0 ? 5 : 1;
            color(g.out ? 0xFF2020 : g.wall ? 0x40FF60 : 0x909090);
            gle::defvertex();
            loopj(strips)
            {
                gle::begin(GL_LINE_STRIP);
                loopvk(x.path) gle::attrib(vec(x.path[k]).sub(feet).add(bold[j]));
                gle::end();
            }
            vec end = vec(x.path[x.path.length()-1]).sub(feet);
            ring(end, 3);
            if(g.first == 0) { ring(end, 3.5f); ring(end, 4); }
        }
        glEnable(GL_DEPTH_TEST);
    }

    static void stuckexitlabels()
    {
        if(stucksince < 0 || stuckcomputed < 0) return;
        loopv(stuckgroups)
        {
            const stuckgroup &g = stuckgroups[i];
            const stuckexit &x = stuckexits[g.first][0];
            const vec &end = x.path[x.path.length()-1];
            defformatstring(label, "%s %d", g.keys, int(end.dist(x.path[0]) + 0.5f));
            if(g.hi - g.lo >= 1) concformatstring(label, " (%d-%d)", int(g.lo + 0.5f), int(g.hi + 0.5f));
            if(g.out) concatstring(label, " out of the map");
            else if(g.wall) concatstring(label, " through wall");
            particle_textcopy(vec(end).sub(vec(0, 0, player1->eyeheight - 4)), label, PART_TEXT, 1, g.out ? 0xFF2020 : g.wall ? 0x40FF60 : 0xB0B0B0, g.first == 0 ? 2.8f : 2.0f);
        }
    }

    // Stuck finder: spots where a player holding W comes to rest without standing and keeps building fall
    // speed. Candidates (from the whole-map scan) are free points with a crack close by; each is tested by
    // holding W facing into it. Some spots hold the player until a key is let go, others only for a moment
    // before it slides on. How high in the crack the player wedges depends on how it got in, so a holding
    // spot keeps the stuck states found up to STUCKLIFTS*STUCKLIFTSTEP units above it; it is marked green when letting go
    // of every key in any of them, at any fall speed from STUCKMINWALLFALL to STUCKMAXWALLFALL, throws the
    // player across geometry without killing it. Spots within STUCKSHOWN are drawn.
    MOD(VARP, practicestuckspots, 0, 1, 1);
    enum { STUCKGRID = 4, STUCKTESTTICKS = 80, STUCKHOLDTICKS = 200, STUCKWANDER = 20, STUCKSTANDING = 10, STUCKWALLTICKS = 200, STUCKSHOWN = 96 };
    enum { STUCKLIFTS = 48, STUCKMAXSTATES = 48, STUCKMINWALLFALL = 1000, STUCKMAXWALLFALL = 16000, STUCKWALLFALLSTEP = 250, STUCKWALLFALLS = (STUCKMAXWALLFALL - STUCKMINWALLFALL)/STUCKWALLFALLSTEP + 1 };
    enum { STUCK_NONE = 0, STUCK_MOMENT, STUCK_HOLDS };
    // the wall check is done a step at a time: first a stuck test per lift (step 1..STUCKLIFTS), then per
    // stuck state and fall speed
    struct stuckspot { vec o; vector<float> yaws; int kind; vector<physent> states; bool checked, wall, lifted, builds; int step; };
    static vector<stuckspot> stuckspots;
    static string stuckspotsmap = "";

    // a copy of the player at start holding W facing yaw: whether it gets stuck and, if so, whether it stays
    // for STUCKHOLDTICKS more (STUCK_HOLDS) or slides on (STUCK_MOMENT); stuck is the state it got stuck in
    static int stucktest(fpsent &sim, const vec &start, float yaw, physent &stuck)
    {
        (physent &)sim = *player1;
        sim.o = start;
        sim.vel = sim.falling = vec(0, 0, 0);
        sim.physstate = PHYS_FALL;
        sim.timeinair = 0;
        sim.floor = vec(0, 0, 1);
        sim.yaw = yaw;
        sim.pitch = 0;
        sim.move = 1;
        sim.strafe = 0;
        sim.jumping = false;
        vec last = sim.o;
        int still = 0, standing = 0;
        vec standpos = sim.o;
        loopi(STUCKTESTTICKS)
        {
            float before = sim.falling.magnitude();
            simulatetick(&sim, player1);
            // gone from where it started: a spot further on is another candidate's to find
            if(vec(sim.o.x - start.x, sim.o.y - start.y, 0).squaredlen() > STUCKWANDER*STUCKWANDER) return STUCK_NONE;
            // standing (or shuffling) on the ground against a wall: that's not being stuck
            if(sim.physstate >= PHYS_FLOOR)
            {
                still = 0;
                last = sim.o;
                if(sim.o.dist(standpos) > 1) { standpos = sim.o; standing = 0; }
                else if(++standing >= STUCKSTANDING) return STUCK_NONE;
                continue;
            }
            standing = 0;
            if(sim.o.dist(last) > 0.01f) { last = sim.o; still = 0; continue; }
            if(sim.falling.magnitude() <= before) { still = 0; continue; }
            if(++still < STUCKSETTLE) continue;
            stuck = sim;
            loopj(STUCKHOLDTICKS)
            {
                simulatetick(&sim, player1);
                if(sim.o.dist(last) > 0.01f) return STUCK_MOMENT;
            }
            return STUCK_HOLDS;
        }
        return STUCK_NONE;
    }

    // Being stuck needs the player's box against at least two surfaces it can't stand on, facing different
    // ways (a sharp corner, a crack, a model against a wall). The box is nudged a little in 8 directions
    // across and 5 downwards from o; true if those bumps meet two such surfaces STUCKANGLE apart.
    enum { STUCKPROBE = 4 };
    enum { STUCKBUILDTICKS = 400, STUCKSTAYTICKS = 20 };
    static const float STUCKLIFTSTEP = 0.25f, STUCKBUILDGROWTH = 0.85f;
    static const float STUCKFLOORZ = 0.867f, STUCKANGLE = 0.94f; // cos 20 degrees
    static bool nearcrack(fpsent &sim, const vec &o, vec *walls = NULL)
    {
        static const vec dirs[13] =
        {
            vec(1, 0, 0), vec(0.7071f, 0.7071f, 0), vec(0, 1, 0), vec(-0.7071f, 0.7071f, 0),
            vec(-1, 0, 0), vec(-0.7071f, -0.7071f, 0), vec(0, -1, 0), vec(0.7071f, -0.7071f, 0),
            vec(0, 0, -1), vec(0.7071f, 0, -0.7071f), vec(-0.7071f, 0, -0.7071f), vec(0, 0.7071f, -0.7071f), vec(0, -0.7071f, -0.7071f)
        };
        vec found[2];
        int n = 0;
        loopi(13)
        {
            sim.o = vec(dirs[i]).mul(STUCKPROBE).add(o);
            if(!collide(&sim, dirs[i]) || collideinside || collidewall.iszero() || collidewall.z >= STUCKFLOORZ) continue;
            vec w = vec(collidewall).normalize();
            bool same = false;
            loopj(n) if(found[j].dot(w) > STUCKANGLE) same = true;
            if(same) continue;
            if(n >= 1)
            {
                sim.o = o;
                if(walls) { walls[0] = found[0]; walls[1] = w; }
                return true;
            }
            found[n++] = w;
        }
        sim.o = o;
        return false;
    }

    static void addyaw(vector<float> &yaws, float yaw)
    {
        loopv(yaws) { float d = fabs(fmodf(yaws[i] - yaw + 540, 360) - 180); if(d < 15) return; }
        yaws.add(yaw);
    }

    // a stuck state of a spot unless one at about the same height is kept already; a spot whose wall check
    // is done goes on with it for the new state
    static void addstuckstate(stuckspot &p, const physent &stuck)
    {
        if(p.states.length() >= STUCKMAXSTATES) return;
        loopv(p.states) if(fabs(p.states[i].o.z - stuck.o.z) < STUCKLIFTSTEP/2) return;
        p.states.add(stuck);
        if(p.checked && !p.wall && p.lifted)
        {
            p.checked = false;
            p.step = STUCKLIFTS + (p.states.length() - 1)*STUCKWALLFALLS;
        }
    }

    static void addstuckspot(const physent &stuck, float yaw, int kind)
    {
        loopv(stuckspots)
        {
            stuckspot &p = stuckspots[i];
            if(vec(p.o.x - stuck.o.x, p.o.y - stuck.o.y, 0).magnitude() >= 2 || fabs(p.o.z - stuck.o.z) >= 8) continue;
            addyaw(p.yaws, yaw);
            if(kind > p.kind)
            {
                p.kind = kind;
                p.states.setsize(0);
                p.states.add(stuck);
                p.checked = p.wall = p.lifted = p.builds = false;
                p.step = 0;
            }
            else if(kind == STUCK_HOLDS) addstuckstate(p, stuck);
            return;
        }
        stuckspot &p = stuckspots.add();
        p.o = stuck.o;
        p.yaws.add(yaw);
        p.kind = kind;
        p.states.add(stuck);
        p.checked = p.wall = p.lifted = p.builds = false;
        p.step = 0;
    }

    // where a throw ended is not a death on its way: inside the map, with ground under it that does not kill
    // (ending inside geometry, partly through a wall, counts)
    static bool notfallingout(const vec &o)
    {
        if(!insideworld(o)) return false;
        float d = raycube(o, vec(0, 0, -1), o.z, RAY_CLIPMAT);
        if(d < 0 || d >= o.z - 0.5f) return false;
        return !(lookupmaterial(vec(o).sub(vec(0, 0, d + 1)))&MAT_DEATH) && !(lookupmaterial(vec(o).sub(vec(0, 0, max(d - 1, 0.0f))))&MAT_DEATH);
    }

    // holding W from a stuck state for ticks: it stays where it is and the fall speed grows by at least
    // growth of what gravity adds (a spot whose fall speed levels off, or that lets go by itself, is no use)
    static bool keepsbuilding(fpsent &sim, const physent &state, int ticks, float growth)
    {
        (physent &)sim = state;
        sim.jumping = false;
        float before = sim.falling.magnitude();
        loopi(ticks)
        {
            simulatetick(&sim, player1);
            if(sim.o.dist(state.o) > 0.05f) return false;
        }
        return sim.falling.magnitude() - before >= growth*GRAVITY*ticks*SIMTICK/1000.0f;
    }

    // one step of a holding spot's wall check: a stuck test from a lift above the spot; or, for one stuck
    // state at one fall speed (lowest first), holding on there and then letting go of every key. A state
    // stops at the first speed it can't be held at; a spot where no state builds speed is dropped.
    static void stepstuckwall(stuckspot &p)
    {
        static fpsent sim;
        if(p.step < STUCKLIFTS)
        {
            vec from = vec(p.o).add(vec(0, 0, (p.step + 1)*STUCKLIFTSTEP));
            (physent &)sim = *player1;
            sim.o = from;
            if(!collide(&sim) && !collideinside) loopv(p.yaws)
            {
                physent stuck;
                if(stucktest(sim, from, p.yaws[i], stuck) == STUCK_HOLDS && vec(stuck.o.x - p.o.x, stuck.o.y - p.o.y, 0).magnitude() < 2)
                    addstuckstate(p, stuck);
            }
            if(++p.step >= STUCKLIFTS) p.lifted = true;
            return;
        }
        int i = p.step - STUCKLIFTS, state = i/STUCKWALLFALLS;
        if(state >= p.states.length())
        {
            p.checked = true;
            if(!p.builds) p.kind = STUCK_NONE;
            return;
        }
        if(i%STUCKWALLFALLS == 0)
        {
            if(!keepsbuilding(sim, p.states[state], STUCKBUILDTICKS, STUCKBUILDGROWTH)) { p.step += STUCKWALLFALLS; return; }
            p.builds = true;
        }
        physent base = p.states[state];
        float fall = base.falling.magnitude(), want = STUCKMINWALLFALL + (i%STUCKWALLFALLS)*STUCKWALLFALLSTEP;
        if(fall > 0) base.falling.mul(want/fall);
        if(!keepsbuilding(sim, base, STUCKSTAYTICKS, 0)) { p.step = STUCKLIFTS + (state + 1)*STUCKWALLFALLS; return; }
        vector<vec> path;
        if(simulateexit(sim, base, 0, 0, 0, &path, STUCKWALLTICKS) && throughwall(path) && notfallingout(sim.o)) { p.wall = p.checked = true; return; }
        p.step++;
    }

    // a found spot not yet given its wall check: one step of it; false if there is none
    static bool checkpendingspot()
    {
        loopv(stuckspots) if(!stuckspots[i].checked)
        {
            // only holding spots can build up the speed a wall clip needs
            if(stuckspots[i].kind == STUCK_HOLDS) stepstuckwall(stuckspots[i]);
            else stuckspots[i].checked = true;
            return true;
        }
        return false;
    }

    static void testcandidate(const vec &start)
    {
        static fpsent sim;
        (physent &)sim = *player1;
        sim.o = start;
        if(collide(&sim) || collideinside) return;
        vec walls[2];
        if(!nearcrack(sim, start, walls)) return;
        // The way into a wedge is narrow (a few units), so the points around a crack are tried too, each
        // facing into the corner between the two surfaces and straight into each of them.
        vec aims[3] = { vec(walls[0]).add(walls[1]).neg(), vec(walls[0]).neg(), vec(walls[1]).neg() };
        float yaws[3];
        loopi(3) yaws[i] = atan2f(-aims[i].x, aims[i].y)/RAD;
        loopk(9)
        {
            vec from(start.x + (k%3 - 1)*STUCKGRID/2, start.y + (k/3 - 1)*STUCKGRID/2, start.z);
            sim.o = from;
            if(k != 4 && (collide(&sim) || collideinside)) continue;
            loopi(3)
            {
                if(aims[i].x == 0 && aims[i].y == 0) continue;
                physent stuck;
                int kind = stucktest(sim, from, yaws[i], stuck);
                if(kind) addstuckspot(stuck, yaws[i], kind);
            }
        }
    }

    // Whole-map scan (practicestuckmapscan): every STUCKGRID column of the map, top to bottom, gives the
    // points just above each surface in it (geometry and mapmodels) as candidates. It runs within
    // STUCKMAPBUDGETUS per frame; the spots it finds are saved per map (by its CRC) and loaded with it.
    enum { STUCKMAPBUDGETUS = 20000, STUCKMAPLAYERS = 10, STUCKMAPLAYERSTEP = 4, STUCKMAPMAXSURFACES = 32 };
    static bool stuckmapscanning = false;
    static int stuckmapcolumn = 0, stuckmapcolumns = 0, stuckmapstart = 0;
    static vector<vec> stuckmapcands;
    static int stuckmapcandpos = 0;

    static const char *stuckmapfile()
    {
        static string file;
        formatstring(file, "stuckspots/%s_%08x.txt", getclientmap(), getmapcrc());
        return path(file, true);
    }

    static void savestuckspots()
    {
        stream *f = openutf8file(stuckmapfile(), "w");
        if(!f) { conoutf(CON_ERROR, "could not write %s", stuckmapfile()); return; }
        f->printf("stuckspots 5\n");
        loopv(stuckspots)
        {
            const stuckspot &p = stuckspots[i];
            if(p.kind == STUCK_NONE) continue;
            f->printf("%.2f %.2f %.2f %d %d", p.o.x, p.o.y, p.o.z, p.kind, p.wall ? 1 : 0);
            loopvj(p.yaws) f->printf(" %.1f", p.yaws[j]);
            f->printf("\n");
        }
        delete f;
    }

    static bool loadstuckspots()
    {
        stream *f = openutf8file(stuckmapfile(), "r");
        if(!f) return false;
        char line[512];
        bool ok = f->getline(line, sizeof(line)) && !strncmp(line, "stuckspots 5", 12);
        while(ok && f->getline(line, sizeof(line)))
        {
            stuckspot &p = stuckspots.add();
            int wall = 0, n = 0;
            char *c = line;
            p.kind = STUCK_NONE;
            if(sscanf(c, "%f %f %f %d %d%n", &p.o.x, &p.o.y, &p.o.z, &p.kind, &wall, &n) < 5) { stuckspots.pop(); continue; }
            p.wall = wall != 0;
            p.checked = p.lifted = p.builds = true;
            p.step = 0;
            for(c += n;;)
            {
                float yaw;
                int m = 0;
                if(sscanf(c, "%f%n", &yaw, &m) < 1) break;
                p.yaws.add(yaw);
                c += m;
            }
        }
        delete f;
        return ok;
    }

    static void mapcolumncandidates(int column)
    {
        int n = getworldsize()/STUCKGRID;
        float x = (column%n)*STUCKGRID + STUCKGRID/2.0f, y = (column/n)*STUCKGRID + STUCKGRID/2.0f, z = getworldsize();
        loopi(STUCKMAPMAXSURFACES)
        {
            float d = raycube(vec(x, y, z), vec(0, 0, -1), z, RAY_CLIPMAT|RAY_POLY);
            if(d < 0 || d >= z - 0.5f) break;
            float hit = z - d;
            loopk(STUCKMAPLAYERS) stuckmapcands.add(vec(x, y, hit + player1->eyeheight + 2 + k*STUCKMAPLAYERSTEP));
            // through whatever is under the surface to the space below it
            float below = hit - 1;
            while(below > 0 && raycube(vec(x, y, below), vec(0, 0, -1), 1, RAY_CLIPMAT|RAY_POLY) < 0.01f) below -= STUCKGRID;
            if(below <= 0) break;
            z = below;
        }
    }

    static bool stepmapscan()
    {
        if(checkpendingspot()) return true;
        if(stuckmapcandpos < stuckmapcands.length()) { testcandidate(stuckmapcands[stuckmapcandpos++]); return true; }
        if(stuckmapcolumn >= stuckmapcolumns)
        {
            stuckmapscanning = false;
            savestuckspots();
            int found = 0;
            loopv(stuckspots) if(stuckspots[i].kind != STUCK_NONE) found++;
            conoutf("stuck spots: %d on %s in %d s, saved", found, getclientmap(), (totalmillis - stuckmapstart)/1000);
            return false;
        }
        stuckmapcands.setsize(0);
        stuckmapcandpos = 0;
        mapcolumncandidates(stuckmapcolumn++);
        return true;
    }

    static void startmapscan()
    {
        stuckspots.setsize(0);
        stuckmapcands.setsize(0);
        stuckmapcandpos = 0;
        stuckmapcolumn = 0;
        int n = getworldsize()/STUCKGRID;
        stuckmapcolumns = n*n;
        stuckmapstart = totalmillis;
        stuckmapscanning = true;
    }

    // practicestuckmapscan: scans the whole map for stuck spots, or stops a scan that is running
    ICOMMAND(practicestuckmapscan, "", (),
    {
        if(stuckmapscanning) { stuckmapscanning = false; conoutf("stuck spot scan stopped"); return; }
        startmapscan();
        conoutf("scanning %s for stuck spots...", getclientmap());
    });
    ICOMMAND(practicestuckscanning, "", (), intret(stuckmapscanning ? 1 : 0));

    static void stuckspotsmapchanged()
    {
        if(!strcmp(stuckspotsmap, getclientmap())) return;
        copystring(stuckspotsmap, getclientmap());
        stuckspots.setsize(0);
        stuckmapscanning = false;
        if(getclientmap()[0]) loadstuckspots();
    }

    static void updatemapscan()
    {
        stuckspotsmapchanged();
        if(!stuckmapscanning) return;
        Uint64 start = SDL_GetPerformanceCounter(), budget = SDL_GetPerformanceFrequency()*STUCKMAPBUDGETUS/1000000;
        keepdynentcache keep;
        mapcollisionsonly maponly;
        while(SDL_GetPerformanceCounter() - start < budget && stepmapscan());
    }

    static void drawstuckspots()
    {
        if(!practicestuckspots) return;
        stuckspotsmapchanged();
        vec feet(0, 0, player1->eyeheight - 0.3f);
        // wedges are inside cracks, so the markers show through the geometry around them
        glDisable(GL_DEPTH_TEST);
        loopv(stuckspots)
        {
            const stuckspot &p = stuckspots[i];
            if(p.kind == STUCK_NONE || p.o.dist(player1->o) > STUCKSHOWN) continue;
            vec at = vec(p.o).sub(feet);
            if(p.kind != STUCK_HOLDS)
            {
                color(0xFFC030);
                ring(at, 1.5f);
                continue;
            }
            color(p.wall ? 0x40FF60 : 0xF0F0F0);
            ring(at, 3);
            gle::defvertex();
            gle::begin(GL_LINES);
            gle::attrib(at);
            gle::attrib(vec(at).add(vec(0, 0, 10)));
            loopvj(p.yaws)
            {
                vec dir(-sinf(p.yaws[j]*RAD), cosf(p.yaws[j]*RAD), 0), tip = vec(dir).mul(8).add(at);
                gle::attrib(at);
                gle::attrib(tip);
                loopk(2)
                {
                    float side = (k ? 150 : -150)*RAD;
                    vec head(dir.x*cosf(side) - dir.y*sinf(side), dir.x*sinf(side) + dir.y*cosf(side), 0);
                    gle::attrib(tip);
                    gle::attrib(head.mul(2.5f).add(tip));
                }
            }
            gle::end();
        }
        glEnable(GL_DEPTH_TEST);
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
    static void drawstuckhud(int w, int h)
    {
        if(stucksince < 0 || !isstuck()) return;
        float fall = player1->falling.magnitude(), side = vec(player1->falling.x, player1->falling.y, 0).magnitude();
        defformatstring(line, "stuck %.1f s: fall %d u/s", (lastmillis - stucksince)/1000.0f, int(fall + 0.5f));
        if(side >= 1) concformatstring(line, " (%d sideways)", int(side + 0.5f));
        concformatstring(line, ", hop %.2f units", fall*(SIMTICK/1000.0f)/10);
        const float scale = 0.4f;
        pushhudmatrix();
        hudmatrix.scale(scale, scale, 1);
        flushhudmatrix();
        int tw = 0, th = 0;
        text_bounds(line, tw, th);
        draw_text(line, int(w/scale/2 - tw/2), int(0.55f*h/scale), 255, 255, 255, 255);
        pophudmatrix();
    }

    static void drawmapscanhud(int w, int h)
    {
        updatemapscan();
        if(!stuckmapscanning) return;
        defformatstring(line, "scanning the map for stuck spots: %d%% (%d found)", stuckmapcolumns ? stuckmapcolumn*100/stuckmapcolumns : 0, stuckspots.length());
        const float scale = 0.4f;
        pushhudmatrix();
        hudmatrix.scale(scale, scale, 1);
        flushhudmatrix();
        int tw = 0, th = 0;
        text_bounds(line, tw, th);
        draw_text(line, int(w/scale/2 - tw/2), int(0.2f*h/scale), 255, 255, 255, 255);
        pophudmatrix();
    }

    void drawpracticehud(int w, int h)
    {
        drawmapscanhud(w, h);
        drawstuckhud(w, h);
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
        drawstuckexits();
        drawstuckspots();
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
        stuckexitlabels();
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
