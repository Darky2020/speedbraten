// serverbrowser.cpp: eihrul's concurrent resolver, and server browser window management

#include "engine.h"

struct resolverthread
{
    SDL_Thread *thread;
    const char *query;
    int starttime;
};

struct resolverresult
{
    const char *query;
    ENetAddress address;
};

vector<resolverthread> resolverthreads;
vector<const char *> resolverqueries;
vector<resolverresult> resolverresults;
SDL_mutex *resolvermutex;
SDL_cond *querycond, *resultcond;

#define RESOLVERTHREADS 2
#define RESOLVERLIMIT 3000

int resolverloop(void * data)
{
    resolverthread *rt = (resolverthread *)data;
    SDL_LockMutex(resolvermutex);
    SDL_Thread *thread = rt->thread;
    SDL_UnlockMutex(resolvermutex);
    if(!thread || SDL_GetThreadID(thread) != SDL_ThreadID())
        return 0;
    while(thread == rt->thread)
    {
        SDL_LockMutex(resolvermutex);
        while(resolverqueries.empty()) SDL_CondWait(querycond, resolvermutex);
        rt->query = resolverqueries.pop();
        rt->starttime = totalmillis;
        SDL_UnlockMutex(resolvermutex);

        ENetAddress address = { ENET_HOST_ANY, ENET_PORT_ANY };
        enet_address_set_host(&address, rt->query);

        SDL_LockMutex(resolvermutex);
        if(rt->query && thread == rt->thread)
        {
            resolverresult &rr = resolverresults.add();
            rr.query = rt->query;
            rr.address = address;
            rt->query = NULL;
            rt->starttime = 0;
            SDL_CondSignal(resultcond);
        }
        SDL_UnlockMutex(resolvermutex);
    }
    return 0;
}

void resolverinit()
{
    resolvermutex = SDL_CreateMutex();
    querycond = SDL_CreateCond();
    resultcond = SDL_CreateCond();

    SDL_LockMutex(resolvermutex);
    loopi(RESOLVERTHREADS)
    {
        resolverthread &rt = resolverthreads.add();
        rt.query = NULL;
        rt.starttime = 0;
        rt.thread = SDL_CreateThread(resolverloop, "resolver", &rt);
    }
    SDL_UnlockMutex(resolvermutex);
}

void resolverstop(resolverthread &rt)
{
    SDL_LockMutex(resolvermutex);
    if(rt.query)
    {
#if SDL_VERSION_ATLEAST(2, 0, 2)
        SDL_DetachThread(rt.thread);
#endif
        rt.thread = SDL_CreateThread(resolverloop, "resolver", &rt);
    }
    rt.query = NULL;
    rt.starttime = 0;
    SDL_UnlockMutex(resolvermutex);
} 

void resolverclear()
{
    if(resolverthreads.empty()) return;

    SDL_LockMutex(resolvermutex);
    resolverqueries.shrink(0);
    resolverresults.shrink(0);
    loopv(resolverthreads)
    {
        resolverthread &rt = resolverthreads[i];
        resolverstop(rt);
    }
    SDL_UnlockMutex(resolvermutex);
}

void resolverquery(const char *name)
{
    if(resolverthreads.empty()) resolverinit();

    SDL_LockMutex(resolvermutex);
    resolverqueries.add(name);
    SDL_CondSignal(querycond);
    SDL_UnlockMutex(resolvermutex);
}

bool resolvercheck(const char **name, ENetAddress *address)
{
    bool resolved = false;
    SDL_LockMutex(resolvermutex);
    if(!resolverresults.empty())
    {
        resolverresult &rr = resolverresults.pop();
        *name = rr.query;
        address->host = rr.address.host;
        resolved = true;
    }
    else loopv(resolverthreads)
    {
        resolverthread &rt = resolverthreads[i];
        if(rt.query && totalmillis - rt.starttime > RESOLVERLIMIT)        
        {
            resolverstop(rt);
            *name = rt.query;
            resolved = true;
        }    
    }
    SDL_UnlockMutex(resolvermutex);
    return resolved;
}

bool resolverwait(const char *name, ENetAddress *address)
{
    if(resolverthreads.empty()) resolverinit();

    defformatstring(text, "resolving %s... (esc to abort)", name);
    renderprogress(0, text);

    SDL_LockMutex(resolvermutex);
    resolverqueries.add(name);
    SDL_CondSignal(querycond);
    int starttime = SDL_GetTicks(), timeout = 0;
    bool resolved = false;
    for(;;) 
    {
        SDL_CondWaitTimeout(resultcond, resolvermutex, 250);
        loopv(resolverresults) if(resolverresults[i].query == name) 
        {
            address->host = resolverresults[i].address.host;
            resolverresults.remove(i);
            resolved = true;
            break;
        }
        if(resolved) break;
    
        timeout = SDL_GetTicks() - starttime;
        renderprogress(min(float(timeout)/RESOLVERLIMIT, 1.0f), text);
        if(interceptkey(SDLK_ESCAPE)) timeout = RESOLVERLIMIT + 1;
        if(timeout > RESOLVERLIMIT) break;    
    }
    if(!resolved && timeout > RESOLVERLIMIT)
    {
        loopv(resolverthreads)
        {
            resolverthread &rt = resolverthreads[i];
            if(rt.query == name) { resolverstop(rt); break; }
        }
    }
    SDL_UnlockMutex(resolvermutex);
    return resolved && address->host != ENET_HOST_ANY;
}

#define CONNLIMIT 20000

int connectwithtimeout(ENetSocket sock, const char *hostname, const ENetAddress &address)
{
    defformatstring(text, "connecting to %s... (esc to abort)", hostname);
    renderprogress(0, text);

    ENetSocketSet readset, writeset;
    if(!enet_socket_connect(sock, &address)) for(int starttime = SDL_GetTicks(), timeout = 0; timeout <= CONNLIMIT;)
    {
        ENET_SOCKETSET_EMPTY(readset);
        ENET_SOCKETSET_EMPTY(writeset);
        ENET_SOCKETSET_ADD(readset, sock);
        ENET_SOCKETSET_ADD(writeset, sock);
        int result = enet_socketset_select(sock, &readset, &writeset, 250);
        if(result < 0) break;
        else if(result > 0)
        {
            if(ENET_SOCKETSET_CHECK(readset, sock) || ENET_SOCKETSET_CHECK(writeset, sock))
            {
                int error = 0;
                if(enet_socket_get_option(sock, ENET_SOCKOPT_ERROR, &error) < 0 || error) break;
                return 0;
            }
        }
        timeout = SDL_GetTicks() - starttime;
        renderprogress(min(float(timeout)/CONNLIMIT, 1.0f), text);
        if(interceptkey(SDLK_ESCAPE)) break;
    }

    return -1;
}
 
#include "serverbrowser.h"

vector<serverinfo *> servers;
ENetSocket pingsock = ENET_SOCKET_NULL;
int lastinfo = 0;

static serverinfo *newserver(const char *name, int port, uint ip = ENET_HOST_ANY)
{
    serverinfo *si = new serverinfo;
    si->address.host = ip;
    si->address.port = server::serverinfoport(port);
    if(ip!=ENET_HOST_ANY) si->resolved = RESOLVED;

    si->port = port;
    if(name) copystring(si->name, name);
    else if(ip==ENET_HOST_ANY || enet_address_get_host_ip(&si->address, si->name, sizeof(si->name)) < 0)
    {
        delete si;
        return NULL;

    }

    servers.add(si);

    return si;
}

void addserver(const char *name, int port, const char *password, bool keep)
{
    if(port <= 0) port = server::serverport();
    loopv(servers)
    {
        serverinfo *s = servers[i];
        if(strcmp(s->name, name) || s->port != port) continue;
        if(password && (!s->password || strcmp(s->password, password)))
        {
            DELETEA(s->password);
            s->password = newstring(password);
        }
        if(keep && !s->keep) s->keep = true;
        return;
    }
    serverinfo *s = newserver(name, port);
    if(!s) return;
    if(password) s->password = newstring(password);
    s->keep = keep;
}

VARP(searchlan, 0, 0, 1);
VARP(servpingrate, 1000, 5000, 60000);
VARP(servpingdecay, 1000, 15000, 60000);
VARP(maxservpings, 0, 10, 1000);

pingattempts lanpings;

template<size_t N> static inline void buildping(ENetBuffer &buf, uchar (&ping)[N], pingattempts &a)
{
    ucharbuf p(ping, N);
    putint(p, a.addattempt(totalmillis));
    buf.data = ping;
    buf.dataLength = p.length();
}

void pingservers()
{
    if(pingsock == ENET_SOCKET_NULL) 
    {
        pingsock = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        if(pingsock == ENET_SOCKET_NULL)
        {
            lastinfo = totalmillis;
            return;
        }
        enet_socket_set_option(pingsock, ENET_SOCKOPT_NONBLOCK, 1);
        enet_socket_set_option(pingsock, ENET_SOCKOPT_BROADCAST, 1);

        lanpings.setoffset();
    }

    ENetBuffer buf;
    uchar ping[MAXTRANS];

    static int lastping = 0;
    if(lastping >= servers.length()) lastping = 0;
    loopi(maxservpings ? min(servers.length(), maxservpings) : servers.length())
    {
        serverinfo &si = *servers[lastping];
        if(++lastping >= servers.length()) lastping = 0;
        if(si.address.host == ENET_HOST_ANY) continue;
        buildping(buf, ping, si);
        enet_socket_send(pingsock, &si.address, &buf, 1);
        
        si.checkdecay(servpingdecay);
    }
    if(searchlan)
    {
        ENetAddress address;
        address.host = ENET_HOST_BROADCAST;
        address.port = server::laninfoport();
        buildping(buf, ping, lanpings);
        enet_socket_send(pingsock, &address, &buf, 1);
    }
    lastinfo = totalmillis;
}
  
void checkresolver()
{
    int resolving = 0;
    loopv(servers)
    {
        serverinfo &si = *servers[i];
        if(si.resolved == RESOLVED) continue;
        if(si.address.host == ENET_HOST_ANY)
        {
            if(si.resolved == UNRESOLVED) { si.resolved = RESOLVING; resolverquery(si.name); }
            resolving++;
        }
    }
    if(!resolving) return;

    const char *name = NULL;
    for(;;)
    {
        ENetAddress addr = { ENET_HOST_ANY, ENET_PORT_ANY };
        if(!resolvercheck(&name, &addr)) break;
        loopv(servers)
        {
            serverinfo &si = *servers[i];
            if(name == si.name)
            {
                si.resolved = RESOLVED; 
                si.address.host = addr.host;
                break;
            }
        }
    }
}

static int lastreset = 0;

void checkpings()
{
    if(pingsock==ENET_SOCKET_NULL) return;
    enet_uint32 events = ENET_SOCKET_WAIT_RECEIVE;
    ENetBuffer buf;
    ENetAddress addr;
    uchar ping[MAXTRANS];
    char text[MAXTRANS];
    buf.data = ping; 
    buf.dataLength = sizeof(ping);
    while(enet_socket_wait(pingsock, &events, 0) >= 0 && events)
    {
        int len = enet_socket_receive(pingsock, &addr, &buf, 1);
        if(len <= 0) return;  
        ucharbuf p(ping, len);
        int millis = getint(p);
        serverinfo *si = NULL;
        loopv(servers) if(addr.host == servers[i]->address.host && addr.port == servers[i]->address.port) { si = servers[i]; break; }
        if(si)
        {
            if(!si->checkattempt(millis)) continue;
            millis = si->decodeping(millis);
        }
        else if(!searchlan || !lanpings.checkattempt(millis, false)) continue;
        else
        {
            si = newserver(NULL, server::serverport(addr.port), addr.host); 
            millis = lanpings.decodeping(millis);
        }
        int rtt = clamp(totalmillis - millis, 0, min(servpingdecay, totalmillis));
        if(millis >= lastreset && rtt < servpingdecay) si->addping(rtt, millis);
        si->numplayers = getint(p);
        int numattr = getint(p);
        si->attr.setsize(0);
        loopj(numattr) { int attr = getint(p); if(p.overread()) break; si->attr.add(attr); }
        getstring(text, p);
        filtertext(si->map, text, false);
        getstring(text, p);
        filtertext(si->sdesc, text, true, true);
    }
}

void sortservers()
{
    servers.sort(serverinfo::compare);
}
COMMAND(sortservers, "");

VARP(autosortservers, 0, 1, 1);
VARP(autoupdateservers, 0, 1, 1);

void refreshservers()
{
    static int lastrefresh = 0;
    if(lastrefresh==totalmillis) return;
    if(totalmillis - lastrefresh > 1000) 
    {
        loopv(servers) servers[i]->reset();
        lastreset = totalmillis;
    }
    lastrefresh = totalmillis;

    checkresolver();
    checkpings();
    if(totalmillis - lastinfo >= servpingrate/(maxservpings ? max(1, (servers.length() + maxservpings - 1) / maxservpings) : 1)) pingservers();
    if(autosortservers) sortservers();
}

serverinfo *selectedserver = NULL;

// Favourite servers are the kept servers (keepserver in servers.cfg: never cleared from the list,
// sorted to the top), sorted into tabs the player names. servers.cfg holds the tabs as favtab lines
// and each favourite's tab as the fourth keepserver argument.
static vector<char *> favtabs;
static serverinfo *favpick = NULL; // a [+] was clicked with several tabs: which one to add it to
static int favrename = -1;         // the tab whose name is being edited

static int findfavtab(const char *name) { loopv(favtabs) if(!strcmp(favtabs[i], name)) return i; return -1; }

static int addfavtab(const char *name)
{
    if(!name[0]) return -1;
    int i = findfavtab(name);
    if(i >= 0) return i;
    favtabs.add(newstring(name, MAXSTRLEN-1));
    return favtabs.length() - 1;
}
ICOMMAND(favtab, "s", (const char *name), addfavtab(name));

static const char *favtabof(const serverinfo &si) { return si.favtab[0] && findfavtab(si.favtab) >= 0 ? si.favtab : favtabs[0]; }

static void setfavourite(serverinfo &si, const char *tab)
{
    si.keep = true;
    copystring(si.favtab, tab);
    writeservercfg();
    // a new favourite is pinged now rather than when its turn comes
    if(pingsock != ENET_SOCKET_NULL && si.address.host != ENET_HOST_ANY)
    {
        ENetBuffer buf;
        uchar ping[MAXTRANS];
        buildping(buf, ping, si);
        enet_socket_send(pingsock, &si.address, &buf, 1);
    }
}

static void removefavourite(serverinfo &si)
{
    si.keep = false;
    si.favtab[0] = '\0';
    writeservercfg();
}

// The favourite column beside rows [start, end) of a server table: [+] adds, [x] removes. The table's
// columns merge hits by row, so a click here also hits its server's row: true then, and the caller
// drops that click.
bool favcolumn(g3d_gui *g, const vector<serverinfo *> &list, int start, int end)
{
    serverinfo *clicked = NULL;
    g->space(2);
    g->pushlist();
    g->text("favourite", 0xFFFF80);
    for(int j = start; j < end; j++) if(g->button(list[j]->keep ? "\f3[x]" : "\f0[+]", 0xFFFFFF)&G3D_UP) clicked = list[j];
    g->poplist();
    if(!clicked) return false;
    if(clicked->keep) removefavourite(*clicked);
    else if(favtabs.length() > 1) favpick = clicked;
    else setfavourite(*clicked, favtabs[0]);
    return true;
}

// while a [+] waits for its tab: one button per tab
void favpickrow(g3d_gui *g)
{
    if(!favpick) return;
    g->pushlist();
    defformatstring(title, "add %s to: ", favpick->sdesc[0] ? favpick->sdesc : favpick->name);
    g->text(title, 0xFFFF80);
    int chosen = -1;
    loopv(favtabs) if(g->button(favtabs[i], 0x80FF80)&G3D_UP) chosen = i;
    g->space(2);
    bool cancel = (g->button("cancel", 0xFF8080)&G3D_UP) != 0;
    g->poplist();
    if(chosen >= 0) setfavourite(*favpick, favtabs[chosen]);
    if(chosen >= 0 || cancel) favpick = NULL;
}

// one tab per favourites tab, each listing its servers with drawlist, and the tab's own buttons
void showfavtabs(g3d_gui *g, uint *header, int pagemin, int pagemax, serverlistfn drawlist, serverinfo *&sc)
{
    if(favtabs.empty()) addfavtab("favourites");
    static vector<serverinfo *> shown;
    int deltab = -1;
    bool newtab = false;
    loopvj(favtabs)
    {
        shown.setsize(0);
        loopv(servers) if(servers[i]->keep && !strcmp(favtabof(*servers[i]), favtabs[j])) shown.add(servers[i]);
        g->tab(favtabs[j], 0xFFDD88);
        if(shown.empty())
        {
            if(header) execute(header);
            g->text("no servers in this tab yet: click [+] beside a server in the server list", 0xFFFFDD);
        }
        else drawlist(g, header, pagemin, pagemax, shown, sc);
        g->pushlist();
        if(favrename == j)
        {
            g->text("new name (enter): ", 0xFFFF80);
            char *name = g->field("favrenametab", 0xFFFFFF, 16, 0, favtabs[j]);
            if(name && name[0] && strcmp(name, favtabs[j]) && findfavtab(name) < 0)
            {
                loopv(servers) if(servers[i]->keep && !strcmp(favtabof(*servers[i]), favtabs[j])) copystring(servers[i]->favtab, name);
                delete[] favtabs[j];
                favtabs[j] = newstring(name, MAXSTRLEN-1);
                favrename = -1;
                writeservercfg();
            }
            if(g->button(" cancel", 0xFF8080)&G3D_UP) favrename = -1;
        }
        else
        {
            if(g->button("rename tab", 0xFFFFDD)&G3D_UP) favrename = j;
            g->space(2);
            if(g->button("+ new tab", 0x80FF80)&G3D_UP) newtab = true;
            if(favtabs.length() > 1)
            {
                g->space(2);
                if(g->button("delete tab", 0xFF8080)&G3D_UP) deltab = j;
            }
        }
        g->poplist();
    }
    if(newtab)
    {
        string name;
        for(int n = favtabs.length() + 1;; n++) { formatstring(name, "favourites %d", n); if(findfavtab(name) < 0) break; }
        favrename = addfavtab(name);
        writeservercfg();
    }
    if(deltab >= 0)
    {
        // the deleted tab's servers stay favourites, in the first remaining tab
        const char *into = favtabs[deltab ? 0 : 1];
        loopv(servers) if(servers[i]->keep && !strcmp(favtabof(*servers[i]), favtabs[deltab])) copystring(servers[i]->favtab, into);
        delete[] favtabs.remove(deltab);
        favrename = -1;
        writeservercfg();
    }
}

static void showserverlist(g3d_gui *cgui, uint *header, int pagemin, int pagemax, const vector<serverinfo *> &list, serverinfo *&sc)
{
    for(int start = 0; start < list.length();)
    {
        if(start > 0) cgui->tab();
        if(header) execute(header);
        if(!start) favpickrow(cgui);
        int end = list.length();
        cgui->pushlist();
        loopi(10)
        {
            if(!game::serverinfostartcolumn(cgui, i)) break;
            for(int j = start; j < end; j++)
            {
                if(!i && j+1 - start >= pagemin && (j+1 - start >= pagemax || cgui->shouldtab())) { end = j; break; }
                serverinfo &si = *list[j];
                const char *sdesc = si.sdesc;
                if(si.address.host == ENET_HOST_ANY) sdesc = "[unknown host]";
                else if(si.ping == serverinfo::WAITING) sdesc = "[waiting for response]";
                if(game::serverinfoentry(cgui, i, si.name, si.port, sdesc, si.map, sdesc == si.sdesc ? si.ping : -1, si.attr, si.numplayers))
                    sc = &si;
            }
            game::serverinfoendcolumn(cgui, i);
        }
        if(favcolumn(cgui, list, start, end)) sc = NULL;
        cgui->poplist();
        start = end;
    }
}

const char *showservers(g3d_gui *cgui, uint *header, int pagemin, int pagemax)
{
    refreshservers();
    serverinfo *sc = NULL;
    if(servers.empty())
    {
        if(header) execute(header);
    }
    else showserverlist(cgui, header, pagemin, pagemax, servers, sc);
    showfavtabs(cgui, header, pagemin, pagemax, showserverlist, sc);
    if(selectedserver || !sc) return NULL;
    selectedserver = sc;
    return "connectselected";
}

void connectselected()
{
    if(!selectedserver) return;
    connectserv(selectedserver->name, selectedserver->port, selectedserver->password);
    selectedserver = NULL;
}

COMMAND(connectselected, "");

void clearservers(bool full = false)
{
    resolverclear();
    if(full) servers.deletecontents();
    else loopvrev(servers) if(!servers[i]->keep) delete servers.remove(i);
    selectedserver = NULL;
    favpick = NULL;
}

#define RETRIEVELIMIT 20000

ENetAddress masteraddress = { ENET_HOST_ANY, ENET_PORT_ANY };

ENetSocket connectmaster(bool wait)
 {
    if(!mastername[0]) return ENET_SOCKET_NULL;
    if(masteraddress.host == ENET_HOST_ANY)
    {
        if(isdedicatedserver()) logoutf("looking up %s...", mastername);
        masteraddress.port = masterport;
        if(!resolverwait(mastername, &masteraddress)) return ENET_SOCKET_NULL;
    }
    ENetSocket sock = enet_socket_create(ENET_SOCKET_TYPE_STREAM);
    if(sock == ENET_SOCKET_NULL)
    {
        if(isdedicatedserver()) logoutf("could not open master server socket");
        return ENET_SOCKET_NULL;
    }
    if(wait || serveraddress.host == ENET_HOST_ANY || !enet_socket_bind(sock, &serveraddress))
    {
        enet_socket_set_option(sock, ENET_SOCKOPT_NONBLOCK, 1);
        if(wait)
        {
            if(!connectwithtimeout(sock, mastername, masteraddress)) return sock;
        }
        else if(!enet_socket_connect(sock, &masteraddress)) return sock;
    }
    enet_socket_destroy(sock);
    if(isdedicatedserver()) logoutf("could not connect to master server");
    return ENET_SOCKET_NULL;
}

void retrieveservers(vector<char> &data)
{
    ENetSocket sock = connectmaster(true);
    if(sock == ENET_SOCKET_NULL) return;

    extern char *mastername;
    defformatstring(text, "retrieving servers from %s... (esc to abort)", mastername);
    renderprogress(0, text);

    int starttime = SDL_GetTicks(), timeout = 0;
    const char *req = "list\n";
    int reqlen = strlen(req);
    ENetBuffer buf;
    while(reqlen > 0)
    {
        enet_uint32 events = ENET_SOCKET_WAIT_SEND;
        if(enet_socket_wait(sock, &events, 250) >= 0 && events) 
        {
            buf.data = (void *)req;
            buf.dataLength = reqlen;
            int sent = enet_socket_send(sock, NULL, &buf, 1);
            if(sent < 0) break;
            req += sent;
            reqlen -= sent;
            if(reqlen <= 0) break;
        }
        timeout = SDL_GetTicks() - starttime;
        renderprogress(min(float(timeout)/RETRIEVELIMIT, 1.0f), text);
        if(interceptkey(SDLK_ESCAPE)) timeout = RETRIEVELIMIT + 1;
        if(timeout > RETRIEVELIMIT) break;
    }

    if(reqlen <= 0) for(;;)
    {
        enet_uint32 events = ENET_SOCKET_WAIT_RECEIVE;
        if(enet_socket_wait(sock, &events, 250) >= 0 && events)
        {
            if(data.length() >= data.capacity()) data.reserve(4096);
            buf.data = data.getbuf() + data.length();
            buf.dataLength = data.capacity() - data.length();
            int recv = enet_socket_receive(sock, NULL, &buf, 1);
            if(recv <= 0) break;
            data.advance(recv);
        }
        timeout = SDL_GetTicks() - starttime;
        renderprogress(min(float(timeout)/RETRIEVELIMIT, 1.0f), text);
        if(interceptkey(SDLK_ESCAPE)) timeout = RETRIEVELIMIT + 1;
        if(timeout > RETRIEVELIMIT) break;
    }

    if(data.length()) data.add('\0');
    enet_socket_destroy(sock);
}

bool updatedservers = false;

void updatefrommaster()
{
    vector<char> data;
    retrieveservers(data);
    if(data.empty()) conoutf(CON_ERROR, "master server not replying");
    else
    {
        clearservers();
        char *line = data.getbuf();
        while(char *end = (char *)memchr(line, '\n', data.length() - (line - data.getbuf())))
        {
            *end = '\0';

            const char *args = line;
            while(args < end && !iscubespace(*args)) args++;
            int cmdlen = args - line;
            while(args < end && iscubespace(*args)) args++;

            if(matchstring(line, cmdlen, "addserver"))
            {
                string ip;
                int port;
                if(sscanf(args, "%100s %d", ip, &port) == 2) addserver(ip, port);
            }
            else if(matchstring(line, cmdlen, "echo")) conoutf("\f1%s", args);

            line = end + 1;
        }
    }
    refreshservers();
    updatedservers = true;
}

void initservers()
{
    selectedserver = NULL;
    if(autoupdateservers && !updatedservers) updatefrommaster();
}

ICOMMAND(addserver, "sis", (const char *name, int *port, const char *password), addserver(name, *port, password[0] ? password : NULL));
ICOMMAND(keepserver, "siss", (const char *name, int *port, const char *password, const char *tab),
{
    addserver(name, *port, password[0] ? password : NULL, true);
    if(*port <= 0) *port = server::serverport();
    if(tab[0]) loopv(servers) if(!strcmp(servers[i]->name, name) && servers[i]->port == *port) { copystring(servers[i]->favtab, tab); break; }
});
ICOMMAND(clearservers, "i", (int *full), clearservers(*full!=0));
COMMAND(updatefrommaster, "");
COMMAND(initservers, "");

void writeservercfg()
{
    if(!game::savedservers()) return;
    stream *f = openutf8file(path(game::savedservers(), true), "w");
    if(!f) return;
    if(favtabs.length())
    {
        f->printf("// tabs of the favourite servers\n\n");
        loopv(favtabs) f->printf("favtab %s\n", escapestring(favtabs[i]));
        f->printf("\n");
    }
    int kept = 0;
    loopv(servers)
    {
        serverinfo *s = servers[i];
        if(s->keep)
        {
            if(!kept) f->printf("// favourite servers, never cleared from the server list\n\n");
            if(s->favtab[0]) f->printf("keepserver %s %d %s %s\n", escapeid(s->name), s->port, escapestring(s->password ? s->password : ""), escapestring(s->favtab));
            else if(s->password) f->printf("keepserver %s %d %s\n", escapeid(s->name), s->port, escapestring(s->password));
            else f->printf("keepserver %s %d\n", escapeid(s->name), s->port);
            kept++;
        }
    }
    if(kept) f->printf("\n");
    f->printf("// servers connected to are added here automatically\n\n");
    loopv(servers) 
    {
        serverinfo *s = servers[i];
        if(!s->keep) 
        {
            if(s->password) f->printf("addserver %s %d %s\n", escapeid(s->name), s->port, escapestring(s->password));
            else f->printf("addserver %s %d\n", escapeid(s->name), s->port);
        }
    }
    delete f;
}

