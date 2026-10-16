// Headless re-simulation (sauer_resim): plays a replay file back as fast as possible and
// prints one JSON line with the result to stdout. Exit code 0 = the replay reproduced its
// recorded hashes and run time, 1 = it did not, 2 = the file could not be played.

#include "game.h"
#include "inputs.h"

namespace game
{
    static string resimfile = "";
    static replayinfo resiminfo;
    static bool resimstarted = false;

    bool resimstart(const char *file)
    {
        inputrun run;
        if(!file || !file[0]) { fprintf(stderr, "usage: sauer_resim [-q<home>] [-x<script>] <replay.sbr>\n"); return false; }
        if(!readreplay(file, &run, resiminfo) || run.empty()) { fprintf(stderr, "could not read replay %s\n", file); return false; }
        copystring(resimfile, file);
        playinputrun(run);
        resimstarted = true;
        return true;
    }

    bool resimdone() { return resimstarted && !inputreplayactive(); }

    static void jsonstring(const char *s)
    {
        putchar('"');
        for(; *s; s++)
        {
            if(*s == '"' || *s == '\\') printf("\\%c", *s);
            else if(uchar(*s) < 0x20) printf("\\u%04x", uchar(*s));
            else putchar(*s);
        }
        putchar('"');
    }

    int resimreport()
    {
        int ticks = 0;
        uint hash[DETHASH_GROUPS];
        int desync = inputreplayresult(ticks, hash);
        bool finished = false;
        int runtime = runtimerresult(finished);
        bool timematch = finished == (resiminfo.finished != 0) && (!finished || runtime == resiminfo.runtime);
        bool ok = !desync && ticks == resiminfo.ticks && timematch;

        printf("{\"file\":");
        jsonstring(resimfile);
        printf(",\"version\":%d,\"map\":", resiminfo.version);
        jsonstring(resiminfo.map);
        printf(",\"maps\":%d,\"ticks\":%d,\"recorded_ticks\":%d", resiminfo.maps, ticks, resiminfo.ticks);
        printf(",\"finished\":%s,\"runtime_ms\":%d", finished ? "true" : "false", runtime);
        printf(",\"recorded_finished\":%s,\"recorded_runtime_ms\":%d", resiminfo.finished ? "true" : "false", resiminfo.runtime);
        printf(",\"desync_tick\":%d,\"hash\":\"", desync);
        loopi(DETHASH_GROUPS) printf("%08x", hash[i]);
        printf("\",\"ok\":%s}\n", ok ? "true" : "false");
        fflush(stdout);
        return ok ? 0 : 1;
    }
}
