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
    enum { HUBRETRY = 10000 };

    struct hubplayer
    {
        uint id, lobby;
        int state, mode;
        string name, map;
    };
    struct hublobby
    {
        uint id, host;
        int mode;
        string map;
        vector<uint> members;
    };
    static vector<hubplayer> hubplayers;
    static vector<hublobby> hublobbies;
    static uint hubmyid = 0;
    static string hubrefusal = "";
    static string hubmyname = "", hubaskedname = "", hubtarget = "", hubchangedat = "";
    static int hubretry = 0, hublaststate = WITNESS_IDLE, hubsentstate = -1, hubsentmode = 0;
    static string hubsentmap = "";
    static bool hubasking = false, hubchecking = false, hubkeychanged = false, hubwelcomed = false, hubreported = false;
    static uchar hubnewkey[32];

    static bool autolobbydue = true;
    static uint playalonglobby = 0;

    static bool hasmember(const hublobby &l, uint id) { loopv(l.members) if(l.members[i] == id) return true; return false; }
    static hubplayer *findplayer(uint id) { loopv(hubplayers) if(hubplayers[i].id == id) return &hubplayers[i]; return NULL; }
    static hublobby *findlobby(uint id) { loopv(hublobbies) if(hublobbies[i].id == id) return &hublobbies[i]; return NULL; }
    static hublobby *mylobby() { loopv(hublobbies) if(hasmember(hublobbies[i], hubmyid)) return &hublobbies[i]; return NULL; }
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

    static void sendstatus()
    {
        int state = clientmap[0] ? HUBSTATE_PLAYING : HUBSTATE_MENU, mode = clientmap[0] ? gamemode : 0;
        if(state == hubsentstate && mode == hubsentmode && !strcmp(clientmap, hubsentmap)) return;
        vector<uchar> p;
        sbr2::byteswriter w(p);
        w.var(state);
        puttext(w, clientmap);
        w.svar(mode);
        if(!hubsend(HM_STATUS, p.getbuf(), p.length())) return;
        hubsentstate = state;
        hubsentmode = mode;
        copystring(hubsentmap, clientmap);
        autolobbydue = true;
    }

    static void mapmessage(int type, const char *map, int mode)
    {
        vector<uchar> p;
        sbr2::byteswriter w(p);
        puttext(w, map);
        w.svar(mode);
        hubsend(type, p.getbuf(), p.length());
    }

    // A race, flagrun or campaign map opens a lobby with the player as host, or moves the
    // player's lobby to it; loading something else leaves the lobby. Playing the map of the
    // lobby one follows keeps following it.
    static void autolobby()
    {
        if(!autolobbydue || !hubmyname[0]) return;
        autolobbydue = false;
        hublobby *l = mylobby();
        bool runmap = clientmap[0] && (m_sp || flagrunrules() || racerules());
        if(!runmap) { if(l) hubsend(HM_LOBBYLEAVE, NULL, 0); return; }
        bool same = l && !strcmp(l->map, clientmap) && l->mode == gamemode;
        if(same) return;
        if(l && l->host == hubmyid) mapmessage(HM_LOBBYMAP, clientmap, gamemode);
        else mapmessage(HM_LOBBYCREATE, clientmap, gamemode);
    }

    static void lobbychanged(const hublobby &before, const hublobby &now, bool wasmine)
    {
        bool mine = hasmember(now, hubmyid);
        if(!mine || now.host == hubmyid) return;
        if(wasmine && !strcmp(before.map, now.map) && before.mode == now.mode) return;
        if(!now.map[0]) return;
        bool running = false;
        runtimerticks(running);
        if(!running && !remote)
        {
            conoutf("\f2following %s to %s (%s)", playername(now.host), now.map, server::modename(now.mode, "?"));
            changemap(now.map, now.mode);
            return;
        }
        conoutf("\f2%s picked %s (%s): \f0/hubgo\f2 or the hub tab to play it", playername(now.host), now.map, server::modename(now.mode, "?"));
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
                    gettext(r, q.name, sizeof(q.name));
                    q.state = int(r.var());
                    gettext(r, q.map, sizeof(q.map));
                    q.mode = int(r.svar());
                    q.lobby = uint(r.var());
                    if(!r.ok) break;
                    hubplayer *old = findplayer(q.id);
                    if(old) *old = q;
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
                    if(!r.ok) break;
                    hublobby *old = findlobby(l.id);
                    bool wasmine = old && hasmember(*old, hubmyid);
                    if(hubwelcomed && playalonglobby != l.id) lobbychanged(old ? *old : l, l, wasmine);
                    if(playalonglobby == l.id && hasmember(l, hubmyid))
                    {
                        playalonglobby = 0;
                        if(l.map[0] && !remote) changemap(l.map, l.mode);
                    }
                    if(old) { old->host = l.host; old->mode = l.mode; copystring(old->map, l.map); old->members.setsize(0); old->members.put(l.members.getbuf(), l.members.length()); }
                    else
                    {
                        hublobby &added = hublobbies.add();
                        added.id = l.id; added.host = l.host; added.mode = l.mode; copystring(added.map, l.map);
                        added.members.put(l.members.getbuf(), l.members.length());
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
        }
    }

    static bool hubready() { return hubstate() == WITNESS_OPEN && hubwelcomed; }

    static void sendchat(int scope, const char *text)
    {
        vector<uchar> p;
        sbr2::byteswriter w(p);
        w.var(scope);
        puttext(w, text);
        hubsend(HM_CHAT, p.getbuf(), p.length());
    }

    // say, away from a game server: to the lobby when others are in it, else to everyone
    bool hubchat(const char *text)
    {
        if(!hubready() || !hubmyname[0]) return false;
        hublobby *l = mylobby();
        sendchat(l && l->members.length() > 1 ? HUBCHAT_LOBBY : HUBCHAT_GLOBAL, text);
        return true;
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
        else if(!strcmp(field, "map")) result(l->map);
        else if(!strcmp(field, "mode")) result(server::modename(l->mode, ""));
        else if(!strcmp(field, "count")) intret(l->members.length());
        else if(!strcmp(field, "members"))
        {
            vector<char> buf;
            loopvj(l->members)
            {
                const char *n = playername(l->members[j]);
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
        mapmessage(HM_LOBBYCREATE, clientmap, clientmap[0] ? gamemode : 0);
    });
    ICOMMAND(hubjoin, "i", (int *id),
    {
        if(!hubready()) { conoutf(CON_ERROR, "not connected to the hub"); return; }
        vector<uchar> p;
        sbr2::byteswriter w(p);
        w.var(uint(*id));
        hubsend(HM_LOBBYJOIN, p.getbuf(), p.length());
    });
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
        mapmessage(HM_LOBBYMAP, clientmap, gamemode);
    });
    ICOMMAND(hubgo, "", (),
    {
        hublobby *l = mylobby();
        if(!l || !l->map[0]) { conoutf(CON_ERROR, "you aren't in a lobby with a map"); return; }
        if(remote) { conoutf(CON_ERROR, "leave the game server first"); return; }
        changemap(l->map, l->mode);
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
