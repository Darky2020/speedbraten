#include "engine.h"

#ifndef SPEEDBRATEN_COMMIT
#define SPEEDBRATEN_COMMIT "unknown"
#endif
#ifndef SPEEDBRATEN_DIRTY
#define SPEEDBRATEN_DIRTY 0
#endif

MOD(SVARP, p1xbratenversion, "");

void lockversion()
{
    ident *id = getident("p1xbratenversion");
    if(!id) { conoutf(CON_ERROR, "p1xbratenversion is undefined!"); return; }
    if(!(id->flags&IDF_READONLY)) id->flags += IDF_READONLY;
}

void migratep1xbraten()
{
    const char *version = "<git-dev>"; // replaced in CI build
    if(naturalsort(p1xbratenversion, version) == -1)
    {
        // we're newer, run migrations
        if(naturalsort(p1xbratenversion, "3.0.0") == -1) {
            // activate CON_NONZEN in all consoles
            if(!(confilter&(1<<14))) confilter += 1<<14;
            if(!(fullconfilter&(1<<14))) fullconfilter += 1<<14;
            if(!(miniconfilter&(1<<14))) miniconfilter += 1<<14;
        }
    }
    setsvar("p1xbratenversion", version);
    lockversion();
    // release builds get their tag in place of the placeholder
    if(version[0] == '<') conoutf("speedbraten development build (%s%s)", SPEEDBRATEN_COMMIT, SPEEDBRATEN_DIRTY ? ", modified" : "");
    else conoutf("speedbraten %s", version);
}
