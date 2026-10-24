// The hub: with hubhost set, the client keeps a channel to the hub (shared/hubproto.h), which
// knows who is online, holds the lobbies and carries chat. The hub's keys are learned the first
// time an address is used, as the witness's are; the hub also names the witness to use.

#include "game.h"
#include "inputs.h"
#include "replayio.h"
#include "hubproto.h"

namespace game
{
    SVARP(hubhost, "");
    VARP(hubport, 1, HUBPORT, 65535);
    SVARP(hubkey, ""); // the hub's channel public key, 64 hex digits
    SVARP(hubkeyfor, ""); // the "host:port" hubkey was learned from
    enum { HUBRETRY = 10000, FOLLOWDELAY = 1500, POSINTERVAL = 50 };

    struct hubplayer
    {
        uint id, lobby;
        int state, mode;
        string name, map, kind;
        bool running;
        int ms, best, pb, statat; // best, pb: -1 none; statat: totalmillis ms arrived at
    };
    struct hublobby
    {
        uint id, host;
        int mode;
        string map, kind;
        vector<uint> members, specs;
    };
    static vector<hubplayer> hubplayers;
    static vector<hublobby> hublobbies;
    static uint hubmyid = 0;
    static string hubrefusal = "";
    static string hubmyname = "", hubaskedname = "", hubtarget = "", hubchangedat = "";
    static int hubretry = 0, hublaststate = WITNESS_IDLE, hubsentstate = -1, hubsentmode = 0;
    static string hubsentmap = "", hubsentkind = "";
    static bool hubasking = false, hubchecking = false, hubkeychanged = false, hubwelcomed = false, hubreported = false;
    static uchar hubnewkey[32];

    static bool autolobbydue = true;
    static uint playalonglobby = 0;
    // a host map change waiting for the follower's run to end, and when a run last ran
    static bool followpending = false;
    static int lastrunning = -FOLLOWDELAY;

    static bool hasmember(const hublobby &l, uint id) { loopv(l.members) if(l.members[i] == id) return true; return false; }
    static hubplayer *findplayer(uint id) { loopv(hubplayers) if(hubplayers[i].id == id) return &hubplayers[i]; return NULL; }
    static hublobby *findlobby(uint id) { loopv(hublobbies) if(hublobbies[i].id == id) return &hublobbies[i]; return NULL; }
    static bool hasspec(const hublobby &l, uint id) { loopv(l.specs) if(l.specs[i] == id) return true; return false; }
    // the lobby the player is in, as a member or as a spectator of one
    static hublobby *mylobby() { loopv(hublobbies) if(hasmember(hublobbies[i], hubmyid) || hasspec(hublobbies[i], hubmyid)) return &hublobbies[i]; return NULL; }
    static const char *playername(uint id) { hubplayer *p = findplayer(id); return p ? p->name : "?"; }

    static void hubreset()
    {
        hubplayers.setsize(0);
        hublobbies.setsize(0);
        hubmyid = 0;
        hubmyname[0] = hubaskedname[0] = hubsentmap[0] = hubrefusal[0] = '\0';
        hubsentstate = -1;
        hubwelcomed = false;
        autolobbydue = true;
        playalonglobby = 0;
        followpending = false;
        lobbyghostsclear();
        spectateend("lost the connection to the hub");
    }

    // text on the wire is UTF-8; the game keeps it in its own 8-bit encoding
    static void puttext(sbr2::byteswriter &w, const char *s)
    {
        int len = strlen(s);
        uchar *buf = new uchar[4*len + 1];
        size_t n = encodeutf8(buf, 4*len, (const uchar *)s, len);
        buf[n] = '\0';
        w.str((const char *)buf);
        delete[] buf;
    }

    static bool gettext(sbr2::bytesreader &r, char *dst, int size)
    {
        char buf[4096];
        if(!r.str(buf, sizeof(buf))) { dst[0] = '\0'; return false; }
        size_t n = decodeutf8((uchar *)dst, size - 1, (const uchar *)buf, strlen(buf));
        dst[n] = '\0';
        return true;
    }

    static void hubsendtext(int type, const char *s)
    {
        vector<uchar> p;
        sbr2::byteswriter w(p);
        puttext(w, s);
        hubsend(type, p.getbuf(), p.length());
    }

    // the kind of run the loaded map is played as
    static const char *runkind()
    {
        if(!clientmap[0]) return "";
        if(m_sp) return m_dmsp ? "dmsp" : "campaign";
        if(flagrunrules()) return "flagrun";
        if(racerules()) return "race";
        return "";
    }

    const char *hubrunkind() { return runkind(); }

    // "flagrun, effic ctf": a flagrun or race names its mode too, since each mode is its own record
    static const char *kindormode(const char *kind, int mode)
    {
        static string buf;
        if(!kind[0]) return server::modename(mode, "?");
        if(strcmp(kind, "flagrun") && strcmp(kind, "race")) return kind;
        formatstring(buf, "%s, %s", kind, server::modename(mode, "?"));
        return buf;
    }

    // A lobby names the map its host's run started on: a campaign run that goes on to its next
    // map stays the same lobby, and a member anywhere in that run is on it.
    static const char *runmap() { const inputattempt *a = inputrunfirst(); return a && a->map[0] ? a->map : clientmap; }
    static int runmode() { const inputattempt *a = inputrunfirst(); return a && a->map[0] ? a->mode : gamemode; }
    static bool onlobbymap(const hublobby &l) { return !strcmp(l.map, runmap()) && l.mode == runmode(); }

    // loads a lobby's map with the rules its host plays it under, unless it is already loaded
    static void loadlobbymap(const hublobby &l)
    {
        if(onlobbymap(l)) return;
        server::forcenextflagrun(!strcmp(l.kind, "flagrun") ? 1 : 0);
        server::forcenextracerules(!strcmp(l.kind, "race") ? 1 : 0);
        changemap(l.map, l.mode);
    }

    static void sendstatus()
    {
        int state = clientmap[0] ? HUBSTATE_PLAYING : HUBSTATE_MENU, mode = clientmap[0] ? gamemode : 0;
        const char *kind = runkind();
        if(state == hubsentstate && mode == hubsentmode && !strcmp(clientmap, hubsentmap) && !strcmp(kind, hubsentkind)) return;
        vector<uchar> p;
        sbr2::byteswriter w(p);
        w.var(state);
        puttext(w, clientmap);
        w.svar(mode);
        w.str(kind);
        if(!hubsend(HM_STATUS, p.getbuf(), p.length())) return;
        hubsentstate = state;
        hubsentmode = mode;
        copystring(hubsentmap, clientmap);
        copystring(hubsentkind, kind);
        autolobbydue = true;
        lobbyghostsclear();
    }

    static void mapmessage(int type, const char *map, int mode)
    {
        vector<uchar> p;
        sbr2::byteswriter w(p);
        puttext(w, map);
        w.svar(mode);
        w.str(runkind());
        hubsend(type, p.getbuf(), p.length());
    }

    // player1's position for the other lobby members' ghosts, while others are there to see it
    static void sendpos()
    {
        static int lastsent = 0;
        if(totalmillis - lastsent < POSINTERVAL || remote || inputreplayactive() || player1->state != CS_ALIVE || !runkind()[0]) return;
        hublobby *l = mylobby();
        if(!l || l->members.length() < 2) return;
        lastsent = totalmillis;
        ghostsample g;
        sampleplayer(g, 0);
        float yaw = fmodf(g.yaw, 360);
        if(yaw < 0) yaw += 360;
        vector<uchar> p;
        sbr2::byteswriter w(p);
        w.svar(int(roundf(g.o.x*16)));
        w.svar(int(roundf(g.o.y*16)));
        w.svar(int(roundf(g.o.z*16)));
        w.var(uint(yaw/360*65536) & 0xFFFF);
        w.var(uint(clamp((g.pitch + 90)/180, 0.0f, 1.0f)*65535));
        w.var(uint(g.flags));
        hubsend(HM_POS, p.getbuf(), p.length());
    }

    // A race, flagrun or campaign map opens a lobby with the player as host, or moves the
    // player's lobby to it; loading something else leaves the lobby. Playing the map of the
    // lobby one follows keeps following it.
    static void autolobby()
    {
        if(!autolobbydue || !hubmyname[0] || inputreplayactive() || spectating()) return;
        autolobbydue = false;
        hublobby *l = mylobby();
        const char *kind = runkind();
        if(!kind[0]) { if(l) hubsend(HM_LOBBYLEAVE, NULL, 0); return; }
        bool same = l && onlobbymap(*l) && (l->host != hubmyid || !strcmp(l->kind, kind));
        if(same) return;
        if(l && l->host == hubmyid) mapmessage(HM_LOBBYMAP, runmap(), runmode());
        else mapmessage(HM_LOBBYCREATE, runmap(), runmode());
    }

    static void lobbychanged(const hublobby &before, const hublobby &now, bool wasmine)
    {
        bool mine = hasmember(now, hubmyid);
        if(!mine || now.host == hubmyid) return;
        if(wasmine && !strcmp(before.map, now.map) && before.mode == now.mode) return;
        if(onlobbymap(now)) return;
        if(!now.map[0]) return;
        bool running = false;
        runtimerticks(running);
        if(running) conoutf("\f2%s picked %s (%s): you follow when this run ends", playername(now.host), runlabel(now.map), kindormode(now.kind, now.mode));
        followpending = true;
    }

    // follows the host's map once the player's run has been over for FOLLOWDELAY
    static void followupdate()
    {
        bool running = false;
        runtimerticks(running);
        if(running) lastrunning = totalmillis;
        if(!followpending || inputreplayactive()) return;
        hublobby *l = mylobby();
        if(!l || l->host == hubmyid || !l->map[0] || remote || onlobbymap(*l)) { followpending = false; return; }
        if(running || totalmillis - lastrunning < FOLLOWDELAY) return;
        followpending = false;
        conoutf("\f2following %s to %s (%s)", playername(l->host), runlabel(l->map), kindormode(l->kind, l->mode));
        loadlobbymap(*l);
    }

    // the run timer for the lobby's scoreboard
    static void sendruntime()
    {
        static bool sentrunning = false;
        static int sentat = 0;
        static uint sentlobby = 0;
        hublobby *l = mylobby();
        if(!l || !hasmember(*l, hubmyid) || inputreplayactive() || spectating()) { sentlobby = 0; return; }
        bool running = false;
        int ms = runtimerticks(running)*SIMTICK;
        if(l->id == sentlobby && running == sentrunning && (!running || totalmillis - sentat < 1000)) return;
        vector<uchar> p;
        sbr2::byteswriter w(p);
        w.var(running ? 1 : 0);
        w.var(max(ms, 0));
        if(!hubsend(HM_RUNTIME, p.getbuf(), p.length())) return;
        sentrunning = running;
        sentat = totalmillis;
        sentlobby = l->id;
    }

    static void hubmessages()
    {
        int type;
        vector<uchar> p;
        while(hubreceive(type, p))
        {
            sbr2::bytesreader r(p.getbuf(), p.length());
            switch(type)
            {
                case -1:
                    hubreset();
                    copystring(hubaskedname, player1->name);
                    hubsendtext(HM_HELLO, player1->name);
                    break;
                case HM_WELCOME:
                {
                    uint id = uint(r.var());
                    string name = "", why = "";
                    gettext(r, name, sizeof(name));
                    gettext(r, why, sizeof(why));
                    if(!r.ok) break;
                    hubmyid = id;
                    copystring(hubrefusal, why);
                    if(strcmp(name, hubmyname) && name[0]) { conoutf("\f0hub: you are %s", name); autolobbydue = true; }
                    copystring(hubmyname, name);
                    if(why[0]) conoutf(CON_ERROR, "hub: %s", why);
                    else if(!name[0]) conoutf(CON_ERROR, "hub: pick a name to chat and join lobbies");
                    hubwelcomed = true;
                    break;
                }
                case HM_WITNESS:
                {
                    string host = "";
                    r.str(host, sizeof(host));
                    int port = int(r.var());
                    uchar keys[64];
                    r.bytes(keys, 64);
                    if(r.ok && host[0]) witnessfromhub(host, port, keys, keys + 32);
                    break;
                }
                case HM_PLAYER:
                {
                    hubplayer q;
                    q.id = uint(r.var());
                    q.running = false;
                    q.ms = q.statat = 0;
                    q.best = q.pb = -1;
                    gettext(r, q.name, sizeof(q.name));
                    q.state = int(r.var());
                    gettext(r, q.map, sizeof(q.map));
                    q.mode = int(r.svar());
                    q.lobby = uint(r.var());
                    gettext(r, q.kind, sizeof(q.kind));
                    if(!r.ok) break;
                    hubplayer *old = findplayer(q.id);
                    if(old)
                    {
                        q.running = old->running; q.ms = old->ms; q.best = old->best; q.pb = old->pb; q.statat = old->statat;
                        *old = q;
                    }
                    else hubplayers.add(q);
                    break;
                }
                case HM_GONE:
                {
                    uint id = uint(r.var());
                    loopv(hubplayers) if(hubplayers[i].id == id) { hubplayers.remove(i); break; }
                    break;
                }
                case HM_LOBBY:
                {
                    hublobby l;
                    l.id = uint(r.var());
                    l.host = uint(r.var());
                    gettext(r, l.map, sizeof(l.map));
                    l.mode = int(r.svar());
                    int n = int(r.var());
                    for(int i = 0; i < n && r.ok && i < 256; i++) l.members.add(uint(r.var()));
                    gettext(r, l.kind, sizeof(l.kind));
                    if(r.left()) { int n = int(r.var()); for(int i = 0; i < n && r.ok && i < 256; i++) l.specs.add(uint(r.var())); }
                    if(!r.ok) break;
                    hublobby *old = findlobby(l.id);
                    bool wasmine = old && hasmember(*old, hubmyid);
                    if(hubwelcomed && playalonglobby != l.id) lobbychanged(old ? *old : l, l, wasmine);
                    if(playalonglobby == l.id && hasmember(l, hubmyid))
                    {
                        playalonglobby = 0;
                        if(l.map[0] && !remote) loadlobbymap(l);
                    }
                    if(old) { old->host = l.host; old->mode = l.mode; copystring(old->map, l.map); copystring(old->kind, l.kind); old->members.setsize(0); old->members.put(l.members.getbuf(), l.members.length()); old->specs.setsize(0); old->specs.put(l.specs.getbuf(), l.specs.length()); }
                    else
                    {
                        hublobby &added = hublobbies.add();
                        added.id = l.id; added.host = l.host; added.mode = l.mode; copystring(added.map, l.map); copystring(added.kind, l.kind);
                        added.members.put(l.members.getbuf(), l.members.length());
                        added.specs.put(l.specs.getbuf(), l.specs.length());
                    }
                    break;
                }
                case HM_LOBBYGONE:
                {
                    uint id = uint(r.var());
                    loopv(hublobbies) if(hublobbies[i].id == id) { hublobbies.remove(i); break; }
                    break;
                }
                case HM_CHATFROM:
                {
                    int scope = int(r.var());
                    r.var();
                    string name = "";
                    gettext(r, name, sizeof(name));
                    char text[1024] = "";
                    gettext(r, text, sizeof(text));
                    if(!r.ok) break;
                    conoutf(CON_CHAT, "\f1[%s] %s:\f0 %s", scope == HUBCHAT_LOBBY ? "lobby" : "global", name, text);
                    break;
                }
                case HM_FINISHED:
                {
                    r.var();
                    string name = "", map = "", kind = "";
                    gettext(r, name, sizeof(name));
                    gettext(r, map, sizeof(map));
                    int mode = int(r.svar());
                    gettext(r, kind, sizeof(kind));
                    int ms = int(r.var());
                    if(r.ok) conoutf(CON_GAMEINFO, "\f1[lobby] \f2%s finished %s (%s) in \f0%s", name, runlabel(map), kindormode(kind, mode), formatruntime(ms));
                    break;
                }
                case HM_POSFROM:
                {
                    uint id = uint(r.var());
                    ghostsample g;
                    g.tick = 0;
                    g.o.x = r.svar()/16.0f;
                    g.o.y = r.svar()/16.0f;
                    g.o.z = r.svar()/16.0f;
                    g.yaw = r.var()*360.0f/65536;
                    g.pitch = r.var()*180.0f/65535 - 90;
                    g.flags = int(r.var());
                    if(r.ok && id != hubmyid && findplayer(id)) lobbyghostsample(id, playername(id), g);
                    break;
                }
                case HM_SPECRUN:
                {
                    uint target = uint(r.var()), run = uint(r.var());
                    string map = "", kind = "";
                    gettext(r, map, sizeof(map));
                    int mode = int(r.svar());
                    gettext(r, kind, sizeof(kind));
                    bool chained = r.var() != 0;
                    if(r.ok && target == spectatingid()) spectaterun(run, map, mode, chained);
                    break;
                }
                case HM_SPECDATA:
                {
                    uint run = uint(r.var());
                    int offset = int(r.var());
                    if(r.ok) spectatedata(run, offset, r.p, r.left());
                    break;
                }
                case HM_SPECEND:
                {
                    uint target = uint(r.var());
                    string why = "";
                    gettext(r, why, sizeof(why));
                    if(r.ok && target == spectatingid()) { spectateend(why); autolobbydue = true; }
                    break;
                }
                case HM_RESULT:
                {
                    string name = "", map = "", kind = "";
                    gettext(r, name, sizeof(name));
                    gettext(r, map, sizeof(map));
                    int mode = int(r.svar());
                    gettext(r, kind, sizeof(kind));
                    int ms = int(r.var());
                    bool record = r.var() != 0;
                    int beat = int(r.var()) - 1;
                    if(!r.ok) break;
                    string time, gain;
                    copystring(time, formatruntime(ms));
                    if(beat >= 0) formatstring(gain, "%s faster", formatruntime(beat - ms));
                    else copystring(gain, "the first verified time");
                    conoutf(CON_GAMEINFO, "\f1[global] \f0%s\f2 %s on %s (%s): \f0%s\f2 (%s)", name, record ? "set a record" : "got a PB", runlabel(map), kindormode(kind, mode), time, gain);
                    break;
                }
                case HM_LOBBYSTAT:
                {
                    uint id = uint(r.var());
                    bool running = r.var() != 0;
                    int ms = int(r.var()), best = int(r.var()) - 1, pb = int(r.var()) - 1;
                    hubplayer *q = findplayer(id);
                    if(!r.ok || !q) break;
                    q->running = running; q->ms = ms; q->best = best; q->pb = pb; q->statat = totalmillis;
                    break;
                }
                case HM_NOTICE:
                {
                    char text[1024] = "";
                    gettext(r, text, sizeof(text));
                    if(r.ok) conoutf("\f2hub: %s", text);
                    break;
                }
            }
        }
    }

    static void savehubkey(const char *addr, const uchar *key)
    {
        string hex;
        sbr2::tohex(key, 32, hex);
        setsvar("hubkey", hex);
        setsvar("hubkeyfor", addr);
    }

    // keeps the channel open while hubhost is set; mirrors witnessupdate's key handling
    void hubupdate()
    {
        defformatstring(addr, "%s:%d", hubhost, hubport);
        uchar key[32], learned[32];
        bool known = !strcmp(hubkeyfor, addr) && sbr2::fromhex(hubkey, key, 32);
        if(hublearnedkey(learned))
        {
            if(hubasking || !known)
            {
                savehubkey(addr, learned);
                conoutf("\f0hub key saved for %s", addr);
                known = sbr2::fromhex(hubkey, key, 32);
                hubretry = totalmillis;
            }
            else if(memcmp(learned, key, 32))
            {
                hubkeychanged = true;
                copystring(hubchangedat, addr);
                memcpy(hubnewkey, learned, 32);
                conoutf(CON_ERROR, "the hub at %s shows a different key than the one saved: check it in the speedrun menu (hub tab)", addr);
            }
            hubasking = hubchecking = false;
        }
        if(hubkeychanged && strcmp(hubchangedat, addr)) hubkeychanged = false;
        const char *target = hubhost[0] && !hubkeychanged ? addr : "";
        int state = hubstate();
        if(strcmp(target, hubtarget))
        {
            copystring(hubtarget, target);
            hubconnect("", hubport, NULL, "speedbraten");
            hubreset();
            hubasking = hubchecking = false;
            hubretry = totalmillis;
            state = hubstate();
        }
        const char *why = huberror();
        bool refused = state == WITNESS_CLOSED && known && !hubchecking && (!strncmp(why, "handshake refused", 17) || strstr(why, "during the handshake"));
        if(state == WITNESS_CLOSED && hublaststate != WITNESS_CLOSED && hubtarget[0] && !refused && !hubchecking)
        {
            if(hublaststate == WITNESS_OPEN) conoutf(CON_ERROR, "lost the connection to the hub: %s", why);
            else if(!hubreported) conoutf(CON_ERROR, "can't reach the hub at %s: %s", addr, why);
            hubreported = true;
            hubreset();
        }
        if(state == WITNESS_OPEN) hubreported = false;
        hublaststate = state;
        if(hubtarget[0] && (state == WITNESS_IDLE || state == WITNESS_CLOSED) && (totalmillis - hubretry >= 0 || refused))
        {
            if(!known || refused)
            {
                hubasking = !known;
                hubchecking = known;
                hubconnect(hubhost, hubport, NULL, "speedbraten");
            }
            else hubconnect(hubhost, hubport, key, "speedbraten");
            hubretry = totalmillis + HUBRETRY;
        }
        hubmessages();
        if(hubstate() == WITNESS_OPEN && hubwelcomed)
        {
            if(strcmp(player1->name, hubaskedname))
            {
                copystring(hubaskedname, player1->name);
                hubsendtext(HM_SETNAME, player1->name);
            }
            sendstatus();
            autolobby();
            followupdate();
            sendpos();
            sendruntime();
            spectateupdate();
        }
    }

    static bool hubready() { return hubstate() == WITNESS_OPEN && hubwelcomed; }

    static bool betterbest(const hubplayer *a, const hubplayer *b)
    {
        if((a->best < 0) != (b->best < 0)) return b->best < 0;
        return a->best < b->best;
    }

    // TAB while in a lobby: its players with their run numbers, then its spectators
    bool hubscoreboard(g3d_gui &g)
    {
        if(remote || !hubready()) return false;
        hublobby *l = mylobby();
        if(!l) return false;
        extern int highlightscore;
        vector<hubplayer *> rows;
        loopv(l->members) if(hubplayer *p = findplayer(l->members[i])) rows.add(p);
        rows.sort(betterbest);

        g.space(.25f);
        g.titlef("%s's lobby", COL_WHITE, NULL, playername(l->host));
        g.pushlist();
        g.spring();
        g.text(kindormode(l->kind, l->mode), COL_WHITE);
        g.space(3);
        g.text(runlabel(l->map), COL_WHITE);
        g.spring();
        g.poplist();
        g.space(.75f);

        #define rowcolor(p) (p->id == hubmyid && highlightscore ? COL_YELLOW : COL_WHITE)
        #define rightjustified(b) { g.pushlist(); g.spring(); b; g.poplist(); }
        g.pushlist();

        g.pushlist();
        g.text("name", COL_GRAY);
        loopv(rows) g.textf("%s%s", rowcolor(rows[i]), NULL, rows[i]->name, rows[i]->id == l->host ? " \f4(host)" : "");
        g.poplist();

        g.space(2);
        g.pushlist();
        g.text("map", COL_GRAY);
        loopv(rows)
        {
            hubplayer *p = rows[i];
            const char *where = p->state == HUBSTATE_MENU ? "menu" : !p->map[0] || !strcmp(p->map, l->map) ? "" : runlabel(p->map);
            g.text(where[0] ? where : "-", where[0] ? COL_GRAY : rowcolor(p));
        }
        g.poplist();

        g.space(2);
        g.pushlist();
        g.strut(6);
        rightjustified(g.text("time", COL_GRAY));
        loopv(rows)
        {
            hubplayer *p = rows[i];
            bool running = p->running;
            int ms = p->ms;
            if(p->id == hubmyid && !spectating()) ms = runtimerticks(running)*SIMTICK;
            else if(running) ms += totalmillis - p->statat;
            if(running || ms > 0) rightjustified(g.text(formatruntime(ms), running ? rowcolor(p) : COL_GRAY))
            else rightjustified(g.text("-", COL_GRAY))
        }
        g.poplist();

        g.space(2);
        g.pushlist();
        g.strut(6);
        rightjustified(g.text("best", COL_GRAY));
        loopv(rows) rightjustified(g.text(rows[i]->best >= 0 ? formatruntime(rows[i]->best) : "-", rowcolor(rows[i])));
        g.poplist();

        g.space(2);
        g.pushlist();
        g.strut(6);
        rightjustified(g.text("PB", COL_GRAY));
        loopv(rows) rightjustified(g.text(rows[i]->pb >= 0 ? formatruntime(rows[i]->pb) : "-", rowcolor(rows[i])));
        g.poplist();

        g.poplist();

        extern int showspectators;
        if(showspectators && l->specs.length())
        {
            g.space(.75f);
            g.textf("%d spectator%s", COL_GRAY, NULL, l->specs.length(), l->specs.length() != 1 ? "s" : "");
            loopv(l->specs)
            {
                if((i%3) == 0) { g.pushlist(); g.text("", COL_WHITE, 0); }
                hubplayer *p = findplayer(l->specs[i]);
                if(p) g.text(p->name, rowcolor(p));
                if(i+1 < l->specs.length() && (i+1)%3) g.space(1);
                else g.poplist();
            }
        }
        #undef rowcolor
        #undef rightjustified
        g.space(.25f);
        return true;
    }

    // the game being played goes to the hub for spectators: on a run map, not a replay
    bool hubstreaming() { return hubready() && !remote && !inputreplayactive() && runkind()[0]; }

    static void sendchat(int scope, const char *text)
    {
        vector<uchar> p;
        sbr2::byteswriter w(p);
        w.var(scope);
        puttext(w, text);
        hubsend(HM_CHAT, p.getbuf(), p.length());
    }

    // say, away from a game server: to the lobby when in one, else to everyone
    bool hubchat(const char *text)
    {
        if(!hubready() || !hubmyname[0]) return false;
        sendchat(mylobby() ? HUBCHAT_LOBBY : HUBCHAT_GLOBAL, text);
        return true;
    }

    void hubrunfinished(int runtime)
    {
        if(!hubready() || !mylobby() || inputreplayactive()) return;
        vector<uchar> p;
        sbr2::byteswriter w(p);
        puttext(w, runmap());
        w.svar(runmode());
        w.str(runkind());
        w.var(max(runtime, 0));
        hubsend(HM_FINISH, p.getbuf(), p.length());
    }

    ICOMMAND(hubsay, "C", (char *text), { if(hubready()) sendchat(HUBCHAT_GLOBAL, text); else conoutf(CON_ERROR, "not connected to the hub"); });
    ICOMMAND(hublobbysay, "C", (char *text), { if(hubready()) sendchat(HUBCHAT_LOBBY, text); else conoutf(CON_ERROR, "not connected to the hub"); });

    ICOMMAND(hubstatus, "", (),
    {
        int state = hubstate();
        if(!hubhost[0]) result("off");
        else if(hubkeychanged) result("the hub shows a different key");
        else if(hubasking && state == WITNESS_CONNECTING) result("asking for the hub's key");
        else if(state == WITNESS_OPEN) result("connected");
        else if(state == WITNESS_CONNECTING) result("connecting");
        else if(state == WITNESS_CLOSED) { defformatstring(why, "not connected: %s", huberror()); result(why); }
        else result("not connected");
    });
    ICOMMAND(hubstatuscolour, "", (),
    {
        int state = hubstate();
        result(!hubhost[0] ? "\f4" : hubkeychanged || state == WITNESS_CLOSED ? "\f3" : state == WITNESS_OPEN ? "\f0" : state == WITNESS_CONNECTING ? "\f2" : "\f4");
    });
    ICOMMAND(hubkeychanged, "", (), intret(hubkeychanged ? 1 : 0));
    ICOMMAND(hubnewkey, "", (), { string hex = ""; if(hubkeychanged) sbr2::tohex(hubnewkey, 32, hex); result(hex); });
    ICOMMAND(hubtrustnewkey, "", (),
    {
        if(!hubkeychanged) return;
        defformatstring(addr, "%s:%d", hubhost, hubport);
        savehubkey(addr, hubnewkey);
        hubkeychanged = false;
        conoutf("\f0hub key saved for %s", addr);
    });
    ICOMMAND(hubforgetkey, "", (),
    {
        setsvar("hubkey", "");
        setsvar("hubkeyfor", "");
        hubkeychanged = false;
        hubtarget[0] = '\0';
    });

    ICOMMAND(hubname, "", (), result(hubmyname));
    ICOMMAND(hubnamerefusal, "", (), result(hubrefusal));
    ICOMMAND(hubmyid, "", (), intret(int(hubmyid)));
    ICOMMAND(hubplayers, "", (),
    {
        vector<char> buf;
        loopv(hubplayers) if(hubplayers[i].name[0])
        {
            defformatstring(id, "%s%u", buf.length() ? " " : "", hubplayers[i].id);
            buf.put(id, strlen(id));
        }
        buf.add('\0');
        result(buf.getbuf());
    });
    ICOMMAND(hubplayer, "is", (int *id, char *field),
    {
        hubplayer *p = findplayer(uint(*id));
        if(!p) { result(""); return; }
        if(!strcmp(field, "name")) result(p->name);
        else if(!strcmp(field, "map")) result(p->map);
        else if(!strcmp(field, "mode")) result(server::modename(p->mode, ""));
        else if(!strcmp(field, "kind")) result(p->kind);
        else if(!strcmp(field, "lobby")) intret(int(p->lobby));
        else if(!strcmp(field, "playing")) intret(p->state != HUBSTATE_MENU ? 1 : 0);
        else if(!strcmp(field, "lobbysize")) { hublobby *l = findlobby(p->lobby); intret(l ? l->members.length() : 0); }
        else if(!strcmp(field, "following")) { hublobby *l = findlobby(p->lobby); result(l && l->host != p->id ? playername(l->host) : ""); }
        else result("");
    });
    ICOMMAND(hublobbies, "", (),
    {
        vector<char> buf;
        loopv(hublobbies)
        {
            defformatstring(id, "%s%u", buf.length() ? " " : "", hublobbies[i].id);
            buf.put(id, strlen(id));
        }
        buf.add('\0');
        result(buf.getbuf());
    });
    ICOMMAND(hublobby, "is", (int *id, char *field),
    {
        hublobby *l = findlobby(uint(*id));
        if(!l) { result(""); return; }
        if(!strcmp(field, "host")) result(playername(l->host));
        else if(!strcmp(field, "map")) result(runlabel(l->map));
        else if(!strcmp(field, "mode")) result(server::modename(l->mode, ""));
        else if(!strcmp(field, "kind")) result(l->kind);
        else if(!strcmp(field, "count")) intret(l->members.length());
        else if(!strcmp(field, "members") || !strcmp(field, "spectators"))
        {
            const vector<uint> &ids = field[0] == 'm' ? l->members : l->specs;
            vector<char> buf;
            loopvj(ids)
            {
                const char *n = playername(ids[j]);
                if(buf.length()) buf.put(", ", 2);
                buf.put(n, strlen(n));
            }
            buf.add('\0');
            result(buf.getbuf());
        }
        else result("");
    });
    ICOMMAND(hubmylobby, "", (), { hublobby *l = mylobby(); intret(l ? int(l->id) : 0); });
    ICOMMAND(hubishost, "", (), { hublobby *l = mylobby(); intret(l && l->host == hubmyid ? 1 : 0); });

    ICOMMAND(hubcreatelobby, "", (),
    {
        if(!hubready()) { conoutf(CON_ERROR, "not connected to the hub"); return; }
        mapmessage(HM_LOBBYCREATE, runmap(), clientmap[0] ? runmode() : 0);
    });
    ICOMMAND(hubjoin, "i", (int *id),
    {
        if(!hubready()) { conoutf(CON_ERROR, "not connected to the hub"); return; }
        vector<uchar> p;
        sbr2::byteswriter w(p);
        w.var(uint(*id));
        hubsend(HM_LOBBYJOIN, p.getbuf(), p.length());
    });
    ICOMMAND(hubspectate, "i", (int *id),
    {
        if(!hubready()) { conoutf(CON_ERROR, "not connected to the hub"); return; }
        if(remote) { conoutf(CON_ERROR, "leave the game server first"); return; }
        hubplayer *p = findplayer(uint(*id));
        if(!p || p->id == hubmyid) { conoutf(CON_ERROR, "no such player"); return; }
        spectatebegin(p->id, p->name, clientmap, gamemode, runkind());
        vector<uchar> b;
        sbr2::byteswriter w(b);
        w.var(p->id);
        hubsend(HM_SPECTATE, b.getbuf(), b.length());
        conoutf("\f2waiting for %s's run", p->name);
    });
    ICOMMAND(hubstopspectate, "", (),
    {
        if(!spectating()) return;
        if(hubready()) { vector<uchar> b; sbr2::byteswriter w(b); w.var(0); hubsend(HM_SPECTATE, b.getbuf(), b.length()); }
        spectatestop();
        autolobbydue = true;
    });
    ICOMMAND(hubspectating, "", (), result(spectatingname()));
    ICOMMAND(hubspectatebehind, "", (), { defformatstring(s, "%.1f", spectatebehind()); result(s); });

    // join the lobby the player is in and load its map
    ICOMMAND(hubplayalong, "i", (int *id),
    {
        if(!hubready()) { conoutf(CON_ERROR, "not connected to the hub"); return; }
        if(remote) { conoutf(CON_ERROR, "leave the game server first"); return; }
        hubplayer *p = findplayer(uint(*id));
        if(!p || !p->lobby) { conoutf(CON_ERROR, "that player isn't playing a map"); return; }
        playalonglobby = p->lobby;
        vector<uchar> b;
        sbr2::byteswriter w(b);
        w.var(p->lobby);
        hubsend(HM_LOBBYJOIN, b.getbuf(), b.length());
    });
    ICOMMAND(hubleave, "", (), { if(hubready()) hubsend(HM_LOBBYLEAVE, NULL, 0); });
    // the host: make the map being played the lobby's map
    ICOMMAND(hubsetlobbymap, "", (),
    {
        if(!hubready() || !clientmap[0]) return;
        mapmessage(HM_LOBBYMAP, runmap(), runmode());
    });
    ICOMMAND(hubgo, "", (),
    {
        hublobby *l = mylobby();
        if(!l || !l->map[0]) { conoutf(CON_ERROR, "you aren't in a lobby with a map"); return; }
        if(remote) { conoutf(CON_ERROR, "leave the game server first"); return; }
        loadlobbymap(*l);
    });
    ICOMMAND(hubreport, "is", (int *id, char *reason),
    {
        if(!hubready()) return;
        vector<uchar> p;
        sbr2::byteswriter w(p);
        w.var(uint(*id));
        puttext(w, reason);
        hubsend(HM_REPORT, p.getbuf(), p.length());
    });
}
