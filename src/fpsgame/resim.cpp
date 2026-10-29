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

    // -V<witness key>: also apply the leaderboard rules (as replayverify, trusting that key) and
    // check a life's saved start; the results are extra JSON fields, the exit code is unchanged
    static string resimkey = "";
    static bool resimrulesread = false;
    static int resimfails = 0, resimwarns = 0, resimmissing = 0;
    static string resimfailed = "";
    void resimverify(const char *witnesskey) { copystring(resimkey, witnesskey); replaylifecheck = true; }

    // -L: the file is still being written. Each line on stdin says it grew ("more") or is
    // final ("end"); EOF before "end" means the run was abandoned. Playback waits whenever it
    // would play a tick the file doesn't hold yet, so it plays exactly as from the whole file.
    static bool resimlive = false, resimcomplete = false, resimnext = false;
    static int resimknown = 0;
    void resimlivemode() { resimlive = true; }

    static bool resimread(bool appendonly)
    {
        inputrun run;
        if(resimcomplete)
        {
            if(!readreplay(resimfile, &run, resiminfo) || run.empty()) return false;
            if(resimkey[0]) resimrulesread = replayrules(resimfile, resimkey, resimfails, resimwarns, resimmissing, resimfailed);
        }
        else
        {
            replayinfo info;
            if(!readreplayprefix(resimfile, run, info, resimknown)) return false;
        }
        if(resimstarted) inputreplayextend(run, resimcomplete, appendonly);
        else if(!run.empty())
        {
            if(!resimnext) playinputrun(run);
            else if(!inputreplaycontinue(run)) { fprintf(stderr, "replay %s does not continue the run before it\n", resimfile); exit(2); }
            inputreplayextend(run, resimcomplete, false);
            resimstarted = true;
        }
        return true;
    }

    static void resimmore(bool appendonly)
    {
        char line[64];
        if(!fgets(line, sizeof(line), stdin)) { fprintf(stderr, "replay %s: abandoned before its end\n", resimfile); exit(2); }
        if(!strncmp(line, "end", 3)) resimcomplete = true;
        // before playback starts the file may not hold a whole chunk yet
        if(!resimread(appendonly) && (resimstarted || resimcomplete)) { fprintf(stderr, "could not read replay %s\n", resimfile); exit(2); }
    }

    static bool resimnextattempt()
    {
        int known = inputreplayattempts();
        while(!resimcomplete && inputreplayattempts() <= known) resimmore(true);
        return true;
    }

    int resimreport();

    // -L, at the end of each run: its JSON line, then the witness either ends the process
    // (EOF) or hands over the next run of the life ("next\t<file>\t<json file>"), which is
    // played on from the state this one ended in.
    static void resimchained()
    {
        int code = resimreport();
        char line[2*MAXSTRLEN];
        for(;;)
        {
            if(!fgets(line, sizeof(line), stdin)) exit(code);
            if(!strncmp(line, "next\t", 5)) break;
        }
        string file = "", out = "";
        const char *p = line + 5;
        int n = strcspn(p, "\t\n");
        copystring(file, p, min(n + 1, MAXSTRLEN));
        p += n;
        if(*p == '\t') { p++; copystring(out, p, min(int(strcspn(p, "\n")) + 1, MAXSTRLEN)); }
        if(out[0] && !freopen(out, "w", stdout)) exit(2);
        copystring(resimfile, file);
        resiminfo = replayinfo();
        resimcomplete = resimrulesread = false;
        resimfails = resimwarns = resimmissing = 0;
        resimfailed[0] = '\0';
        resimknown = 0;
        resimstarted = false;
        resimnext = true;
        resimread(false);
        while(!resimstarted) resimmore(false);
    }

    void resimwait()
    {
        if(!resimlive) return;
        while(!resimcomplete && inputreplayneeds(resimknown)) resimmore(false);
        while(!resimcomplete && inputreplayoverrun()) resimmore(true);
    }

    bool resimstart(const char *file)
    {
        inputrun run;
        if(!file || !file[0]) { fprintf(stderr, "usage: sauer_resim [-q<home>] [-x<script>] [-L] <replay.sbr>\n"); return false; }
        copystring(resimfile, file);
        if(resimlive)
        {
            replaywaitattempt = resimnextattempt;
            replaychainhook = resimchained;
            resimread(false);
            while(!resimstarted) resimmore(false);
            return true;
        }
        if(!readreplay(file, &run, resiminfo) || run.empty()) { fprintf(stderr, "could not read replay %s\n", file); return false; }
        if(resimkey[0]) resimrulesread = replayrules(file, resimkey, resimfails, resimwarns, resimmissing, resimfailed);
        playinputrun(run);
        resimstarted = true;
        return true;
    }

    bool resimdone()
    {
        if(!resimstarted || inputreplayactive()) return false;
        while(resimlive && !resimcomplete) resimmore(true);
        return true;
    }

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
        printf("\",\"ok\":%s", ok ? "true" : "false");
        if(resimkey[0])
        {
            printf(",\"rules_read\":%s,\"rules_failed\":%d,\"rules_warned\":%d,\"missing_files\":%d,\"failed\":", resimrulesread ? "true" : "false", resimfails, resimwarns, resimmissing);
            jsonstring(resimfailed);
            printf(",\"lifestart\":");
            jsonstring(replaylifecheckresult);
        }
        printf("}\n");
        fflush(stdout);
        return ok ? 0 : 1;
    }
}
