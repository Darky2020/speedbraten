#include "game.h"

// Online life: racing on a server while the player's own life is played and recorded offline.
bool lifesession = false, lifelocal = false;
extern ENetPeer *curpeer;

namespace game
{
    // the client state the life's world and the server's world each have their own copy of
    extern vector<uchar> messages;
    extern int messagecn, messagereliable, sessionid, mastermode, gamespeed, lastping;
    extern bool connected, remote, gamepaused, sendcrc, senditemstoserver;

    // the map the server sent (lifesentmap) and the file it was saved to (lifesentfile), kept
    // while the server plays it so that lives can load it under its real name
    string lifesentfile = "", lifesentmap = "";

    // A local life: while the player races on a server, their own life is played on the
    // in-process server as an offline game (fixed ticks, race rules, recorded and judged like
    // any offline life) and the server connection carries everything else. The two worlds
    // share player1 and the loaded map; what differs between them is swapped: the life's
    // world is active inside sim ticks and while the in-process server's packets are parsed,
    // the server's world the rest of the time, so a life's saved state holds nothing of the
    // server's players. Only coop edit, the mode race servers run.
    MOD(VARP, onlinelife, 0, 0, 1);

    struct lifeworld
    {
        vector<fpsent *> clients, players;
        vector<uchar> messages;
        int messagecn, messagereliable, clientnum, lifesequence, privilege, sessionid, mastermode, gamespeed, lastping;
        bool connected, remote, gamepaused, sendcrc, senditemstoserver;
    };
    static lifeworld lifeother; // the world that isn't active

    template<class T> static void swapvector(vector<T> &a, vector<T> &b) { vector<T> t; t.move(a); a.move(b); b.move(t); }

    static void swapworlds()
    {
        lifeworld &o = lifeother;
        swapvector(clients, o.clients);
        swapvector(players, o.players);
        swapvector(messages, o.messages);
        swap(messagecn, o.messagecn);
        swap(messagereliable, o.messagereliable);
        swap(player1->clientnum, o.clientnum);
        swap(player1->lifesequence, o.lifesequence);
        swap(player1->privilege, o.privilege);
        swap(sessionid, o.sessionid);
        swap(mastermode, o.mastermode);
        swap(gamespeed, o.gamespeed);
        swap(lastping, o.lastping);
        swap(connected, o.connected);
        swap(remote, o.remote);
        swap(gamepaused, o.gamepaused);
        swap(sendcrc, o.sendcrc);
        swap(senditemstoserver, o.senditemstoserver);
    }

    bool lifepaused() { return lifeother.gamepaused; }
    int lifegamespeed() { return lifeother.gamespeed; }

    // the dynent cache is part of the simulation: the life's is kept while the server's
    // world is active, whose players must never be found in it
    static vector<int> lifedyncache;

    void lifeenter()
    {
        if(!lifesession || lifelocal) return;
        swapworlds();
        lifelocal = true;
        if(lifedyncache.empty() || !loaddynentcache(lifedyncache)) cleardynentcache();
    }

    void lifeleave()
    {
        if(!lifelocal) return;
        lifedyncache.setsize(0);
        if(!savedynentcache(lifedyncache)) lifedyncache.setsize(0);
        swapworlds();
        lifelocal = false;
    }

    // parsing the real server's messages while a life is played: the player's life, items,
    // shots and clock are the life's world's, not the server's
    bool lifebeside() { return lifesession && !lifelocal; }

    // the server killed the player (its podium, a forced death): the life ends the same way,
    // as a recorded suicide so that the life's replay plays it back
    void lifekilled()
    {
        if(player1->state == CS_ALIVE) queueinput(INPUT_COMMAND, 0, "suicide");
    }

    // where a message the life's world sends goes: its own in-process server, the real one, or both
    enum { LIFEMSG_HERE = 0, LIFEMSG_OTHER, LIFEMSG_BOTH };
    static int lifemsgdest(int type)
    {
        switch(type)
        {
            case N_SPAWN:
            case N_ADDBOT: case N_DELBOT: case N_BOTLIMIT: case N_BOTBALANCE:
                return LIFEMSG_HERE;
            case N_SUICIDE: case N_TRYSPAWN: case N_GUNSELECT: case N_SOUND: case N_TAUNT:
            case N_SHOOT: case N_EXPLODE: case N_ITEMPICKUP:
                return LIFEMSG_BOTH;
            default:
                return LIFEMSG_OTHER;
        }
    }

    static void queuemsg(vector<uchar> &q, int &qcn, int &qreliable, const uchar *buf, int len, int mcn, bool reliable)
    {
        if(reliable) qreliable = true;
        if(mcn != qcn)
        {
            uchar mbuf[16];
            ucharbuf m(mbuf, sizeof(mbuf));
            putint(m, N_FROMAI);
            putint(m, mcn);
            q.put(mbuf, m.length());
            qcn = mcn;
        }
        q.put(buf, len);
    }

    // A shot or explosion the server is shown, so that its players and spectators see it: the same message
    // with no hits, since the hits name the life's players by the life's client numbers. Returns its length
    // in out; 0 for other messages or one that doesn't parse.
    static int withouthits(int type, const uchar *buf, int len, uchar *out, int size)
    {
        int fields = type == N_SHOOT ? 8 : type == N_EXPLODE ? 3 : -1;
        if(fields < 0) return 0;
        ucharbuf in((uchar *)buf, len);
        getint(in);
        loopi(fields) getint(in);
        if(in.overread() || in.len + 4 > size) return 0;
        memcpy(out, buf, in.len);
        ucharbuf o(out + in.len, size - in.len);
        putint(o, 0);
        return in.len + o.len;
    }

    // addmsg's queueing: outside a life everything goes to the one server; inside, by message type
    void lifequeuemsg(int type, const uchar *buf, int len, int mcn, bool reliable)
    {
        int dest = lifelocal ? lifemsgdest(type) : LIFEMSG_HERE;
        if(dest != LIFEMSG_OTHER) queuemsg(messages, messagecn, messagereliable, buf, len, mcn, reliable);
        if(dest == LIFEMSG_HERE) return;
        uchar stripped[MAXTRANS];
        int slen = withouthits(type, buf, len, stripped, sizeof(stripped));
        if(slen) queuemsg(lifeother.messages, lifeother.messagecn, lifeother.messagereliable, stripped, slen, mcn, reliable);
        else if(type != N_SHOOT && type != N_EXPLODE) queuemsg(lifeother.messages, lifeother.messagecn, lifeother.messagereliable, buf, len, mcn, reliable);
    }

    // A life loads the map from the player's own file, which must be the world the server
    // plays: none when the file is missing or the world is one the server sent.
    bool lifeworldsent = false;

    void lifedropsent()
    {
        if(!lifesentfile[0]) return;
        defformatstring(fname, "packages/base/%s.ogz", lifesentfile);
        remove(findfile(path(fname), "rb"));
        lifesentfile[0] = lifesentmap[0] = '\0';
    }

    bool lifemapfile(const char *name)
    {
        if(lifesentmap[0] && !strcmp(name, lifesentmap)) return true;
        defformatstring(fname, "packages/base/%s.ogz", name);
        path(fname);
        return fileexists(fname, "e") || findfile(fname, "e");
    }

    // the server this client is connected to, as "label (host:port)" when the hub lists it
    static bool listedserver(string &out)
    {
        if(!curpeer) return false;
        const char *label = hubserverlabel(curpeer->address);
        if(!label) return false;
        string ip;
        if(enet_address_get_host_ip(&curpeer->address, ip, sizeof(ip)) < 0) copystring(ip, "?");
        formatstring(out, "%s (%s:%d)", label, ip, curpeer->address.port);
        return true;
    }

    // the server the current online life is played on
    static string lifeserver = "";
    const char *onlinelifeserver() { return lifesession ? lifeserver : ""; }

    bool lifewanted(const char *name, int mode)
    {
        if(!onlinelife || !remote || lifesession || !m_check(mode, M_EDIT) || player1->state == CS_SPECTATOR || !name[0]) return false;
        string server;
        if(!listedserver(server)) return false;
        return !lifeworldsent && lifemapfile(name);
    }

    // Called in the server's world, on its map change instead of loading the map, or when it
    // spawns the player outside a life: the life's world loads the map the way an offline
    // game does.
    void onlinelifestart(const char *mapname, int mode)
    {
        string name;
        copystring(name, mapname);
        lifeworld &o = lifeother;
        o.clients.setsize(0);
        o.players.setsize(0);
        o.players.add(player1);
        o.messages.setsize(0);
        o.messagecn = o.clientnum = -1;
        o.messagereliable = false;
        o.lifesequence = 0;
        o.privilege = PRIV_NONE;
        o.sessionid = 0;
        o.mastermode = MM_OPEN;
        o.gamespeed = 100;
        o.lastping = 0;
        o.connected = o.remote = o.gamepaused = o.sendcrc = o.senditemstoserver = false;
        if(!listedserver(lifeserver)) lifeserver[0] = '\0';
        lifesession = true;
        lifeenter();
        server::forcemap(name, mode);
        lifeconnect();
        lifeleave();
        conoutf("local life on %s", name);
    }

    void lifeend(const char *why)
    {
        if(!lifesession) return;
        lifeenter();
        inputdisconnect();
        lifedisconnect();
        lifeleave();
        lifesession = false;
        simticking = false;
        lifeother.players.setsize(0);
        lifeother.clients.setsize(0);
        lifeother.messages.setsize(0);
        conoutf("local life ended: %s", why);
    }

    // the server changed the map: a life on it starts instead of loading the map (true), or the map
    // loads as usual (false)
    bool onlinelifemapchange(const char *name, int mode, bool items)
    {
        if(!remote) return false;
        lifeend("the map changed");
        lifeworldsent = false;
        lifedropsent();
        if(onlinelife && m_check(mode, M_EDIT))
        {
            string server;
            if(!listedserver(server)) conoutf(CON_WARN, "no local life: this server isn't on the hub's list");
            else if(!lifemapfile(name)) conoutf(CON_WARN, "no local life: %s is not in your packages", name);
        }
        if(!lifewanted(name, mode)) return false;
        sendcrc = true;
        senditemstoserver = items;
        onlinelifestart(name, mode);
        return true;
    }

    // the server reports a death during a life: the player's own becomes a recorded suicide, others only
    // keep their frags (true: the message is handled)
    bool onlinelifedied(fpsent *victim, fpsent *actor, int frags)
    {
        if(!lifebeside()) return false;
        if(victim == player1) lifekilled();
        if(actor != player1) actor->frags = frags;
        return true;
    }

    // a map the server sent was saved to file and loaded: lives on it load that file (true: keep it)
    bool onlinelifesentmap(const char *file, const char *name)
    {
        if(!remote || !onlinelife || !name[0]) return false;
        copystring(lifesentfile, file);
        copystring(lifesentmap, name);
        lifeworldsent = false;
        if(player1->state == CS_ALIVE && lifewanted(name, gamemode)) onlinelifestart(name, gamemode);
        return true;
    }

    void parsepacketremote(int chan, packetbuf &p)
    {
        bool live = lifelocal;
        lifeleave();
        parsepacketclient(chan, p);
        if(live) lifeenter();
    }

    void parsepacketlocal(int chan, packetbuf &p)
    {
        bool live = lifelocal;
        lifeenter();
        parsepacketclient(chan, p);
        if(!live) lifeleave();
    }
}
