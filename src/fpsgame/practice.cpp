// Practice helpers: what the game tests the player against, and the health of monsters and
// barrels, drawn outside edit mode. practicehelpers (an assist, off at every launch) turns them on; the
// practice* toggles pick which. Flags, jumppads, teleports and level triggers act when the feet
// enter a sphere around them; a race finish cube is tested against the eye, so it is drawn one eye
// height lower, where the body has to be. A spawn box is the player's bounding box with the eye
// on the spawn point, where spawning puts it. The engine draws the clip, noclip, death and
// gameclip material (practiceclip and the like).

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
        if(!practicehelpers || editmode) return;
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
        if(!practicehelpers || editmode || lastframe == totalmillis || !canaddparticles()) return;
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
        if(practicebarrels) loopi(nummovableslots())
        {
            int health, maxhealth;
            dynent *m = movableslot(i, health, maxhealth);
            if(m->state != CS_ALIVE || maxhealth <= 0) continue;
            healthlabel(vec(m->o).add(vec(0, 0, m->aboveeye + 1)), health, maxhealth);
        }
    }
}
