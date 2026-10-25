// Names speedbraten's own sources take from the client they are built into. Each has a default
// for a client that doesn't define it; a client can define its own before including this.

// a persistent variable saved in the client's mod config rather than config.cfg
#ifndef MOD
#define MOD(macro, name, ...) macro(name, ##__VA_ARGS__)
#endif

#ifndef COL_WHITE
#define COL_WHITE 0xFFFFFF
#endif
#ifndef COL_GRAY
#define COL_GRAY 0xA0A0A0
#endif
#ifndef COL_YELLOW
#define COL_YELLOW 0xFFC040
#endif

// the client's name towards the witness and the hub, and in replay files
#ifndef SPEEDBRATEN_CLIENT
#define SPEEDBRATEN_CLIENT "speedbraten"
#endif

// the cubescript string variable holding the release version
#ifndef SPEEDBRATEN_VERSIONVAR
#define SPEEDBRATEN_VERSIONVAR "p1xbratenversion"
#endif
