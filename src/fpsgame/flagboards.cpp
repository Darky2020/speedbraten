// Flag boards: in a flagrun, floating text above each flag base with the best run taken from that
// flag, the map's PB, the run count and, through the hub, the witnessed board's record, its next
// two places and the player's own. Hidden while a run is going. Like all particle text, they show
// within maxparticletextdistance of the camera.

#include "game.h"
#include "sbcompat.h"
#include "inputs.h"

namespace game
{
    MOD(VARP, flagboards, 0, 0, 1);
    MOD(FVARP, flagboardheight, 0, 40, 200); // above the flag's base
    MOD(FVARP, flagboardsize, 2.0f, 8.0f, 30.0f);

    extern bool hubboardrow(int i, int &rank, int &ms, const char *&name);
    extern bool hubboardmine(int &rank, int &ms);
    extern bool hubboardlegacy(int &ms, const char *&name, const char *&source);

    struct flagboard
    {
        vec o;
        int side;
        vector<char *> lines; // top line first
    };
    static vector<flagboard> boards;
    static string boardkey = "";
    static int boardbuilt = 0;

    static void clearboards()
    {
        loopv(boards) boards[i].lines.deletearrays();
        boards.setsize(0);
    }

    static void addline(flagboard &b, const char *fmt, ...) PRINTFARGS(2, 3);
    static void addline(flagboard &b, const char *fmt, ...)
    {
        defvformatstring(line, fmt, fmt);
        b.lines.add(newstring(line));
    }

    static void buildboards(const char *map, const char *kind, int mode, bool segmented)
    {
        clearboards();
        runsummary summary;
        statsrunsummary(map, kind, mode, segmented, summary);
        // flags are numbered as the CTF mode adds them: the map's flag entities of team 1 or 2
        const vector<extentity *> &ents = entities::getents();
        int side = 0;
        loopv(ents)
        {
            const extentity &e = *ents[i];
            if(e.type != FLAG || e.attr2 < 1 || e.attr2 > 2) continue;
            flagboard &b = boards.add();
            b.o = e.o;
            b.side = ++side;
            int best = statssidebest(map, kind, mode, segmented, b.side);
            addline(b, "\f7%s flagrun on %s", server::modename(mode, "?"), map);
            addline(b, "\f4from this flag  \f7%s", best >= 0 ? formatruntime(best) : "-");
            addline(b, "\f4map PB  \f7%s", summary.pb >= 0 ? formatruntime(summary.pb) : "-");
            addline(b, "\f4runs  \f7%d / %d", summary.finished, summary.attempts);
            // the board, when the hub sent it: the unwitnessed legacy record, the first three
            // places in gold, silver and bronze, and the player's own place below them
            int rank, ms;
            const char *name, *source;
            if(hubboardlegacy(ms, name, source)) addline(b, "\f4legacy  %s  %s", formatruntime(ms), name);
            loopj(3)
            {
                if(!hubboardrow(j, rank, ms, name)) break;
                addline(b, "%s#%d  \f7%s  %s", rank == 1 ? "\f2" : rank == 2 ? "\f9" : rank == 3 ? "\f6" : "\f4", rank, formatruntime(ms), name);
            }
            if(hubboardmine(rank, ms) && rank > 3) addline(b, "\f4you  #%d  \f7%s", rank, formatruntime(ms));
        }
    }

    void renderflagboards()
    {
        const char *kind = runtimerkindname();
        bool running = false;
        runtimerticks(running);
        if(!flagboards || !kind || strcmp(kind, "flagrun") || running) return;
        const inputattempt *first = inputrunfirst();
        const char *map = first ? first->map : getclientmap();
        int mode = first ? first->mode : gamemode;
        bool segmented = inputsplicecount() > 0;
        defformatstring(key, "%s %d %d %d", map, mode, segmented ? 1 : 0, statsversion());
        // the hub's board arrives some time after the map loads
        if(strcmp(key, boardkey) || totalmillis - boardbuilt > 1000)
        {
            copystring(boardkey, key);
            boardbuilt = totalmillis;
            buildboards(map, kind, mode, segmented);
        }
        float step = flagboardsize*0.95f;
        loopv(boards)
        {
            const flagboard &b = boards[i];
            vec o = vec(b.o).add(vec(0, 0, flagboardheight + step*(b.lines.length()-1)));
            loopvj(b.lines)
            {
                particle_textcopy(o, b.lines[j], PART_TEXT, 1, 0xFFFFFF, flagboardsize);
                o.z -= step;
            }
        }
    }
}
