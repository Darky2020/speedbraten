template<int N> static void hashprojectilestate(dethasher &h, const projectilestate<N> &p)
{
    h.i(p.numprojs);
    loopi(p.numprojs) h.i(p.projs[i]);
}

// everything the local server keeps between ticks that later shapes the game
void hashstate(dethasher &h)
{
    h.i(gamemillis); h.i(gamelimit); h.i(interm); h.i(gamemode); h.i(gamepaused);
    h.i(flagrunon); h.i(raceruleson);
    h.i(sents.length());
    loopv(sents) { h.i(sents[i].type); h.i(sents[i].spawned); h.i(sents[i].spawntime); }
    h.i(clients.length());
    loopv(clients)
    {
        const clientinfo &ci = *clients[i];
        const gamestate &gs = ci.state;
        h.i(ci.clientnum); h.i(ci.ownernum); h.i(ci.timesync); h.i(ci.gameoffset); h.i(ci.lastevent);
        h.i(gs.state); h.i(gs.editstate); h.v(gs.o);
        h.i(gs.health); h.i(gs.maxhealth); h.i(gs.armour); h.i(gs.armourtype); h.i(gs.quadmillis);
        h.i(gs.gunselect); h.i(gs.gunwait);
        loopj(NUMGUNS) h.i(gs.ammo[j]);
        h.i(gs.lastdeath); h.i(gs.deadflush); h.i(gs.lastspawn); h.i(gs.lifesequence); h.i(gs.lastshot);
        hashprojectilestate(h, gs.rockets); hashprojectilestate(h, gs.grenades);
        h.i(gs.frags); h.i(gs.flags); h.i(gs.deaths); h.i(gs.teamkills); h.i(gs.shotdamage); h.i(gs.damage);
        h.i(ci.events.length());
        loopvj(ci.events) ci.events[j]->hash(h);
#ifndef STANDALONE
        if(game::dethashdetail) conoutf("dethashdetail server client %d: owner %d timesync %d gameoffset %d lastevent %d state %d health %d armour %d gun %d gunwait %d lastdeath %d deadflush %d lastspawn %d lifeseq %d lastshot %d frags %d events %d o %.3f %.3f %.3f",
            ci.clientnum, ci.ownernum, ci.timesync, ci.gameoffset, ci.lastevent, gs.state, gs.health, gs.armour, gs.gunselect, gs.gunwait, gs.lastdeath, gs.deadflush, gs.lastspawn, gs.lifesequence, gs.lastshot, gs.frags, ci.events.length(), gs.o.x, gs.o.y, gs.o.z);
#endif
    }
    if(smode == &ctfmode)
    {
        loopv(ctfmode.flags)
        {
            const ctfservmode::flag &f = ctfmode.flags[i];
            h.i(f.id); h.i(f.version); h.i(f.spawnindex); if(f.droptime) h.v(f.droploc); h.v(f.spawnloc);
            h.i(f.team); h.i(f.droptime); h.i(f.owntime); h.i(f.owner); h.i(f.dropcount); h.i(f.dropper);
        }
        h.i(ctfmode.scores[0]); h.i(ctfmode.scores[1]);
    }
}

static bool teamnameless(const teaminfo *a, const teaminfo *b) { return strcmp(a->team, b->team) < 0; }

static gameevent *newsnapevent(int type)
{
    switch(type)
    {
        case 1: return new timedevent;
        case 2: return new shotevent;
        case 3: return new explodeevent;
        case 4: return new suicideevent;
        case 5: return new pickupevent;
        default: return NULL;
    }
}

static void snapgamestate(snapio &s, gamestate &gs)
{
    s.state(gs);
    s.v(gs.o); s.i(gs.state); s.i(gs.editstate);
    s.i(gs.lastdeath); s.i(gs.deadflush); s.i(gs.lastspawn); s.i(gs.lifesequence); s.i(gs.lastshot);
    loopi(8) { s.i(gs.rockets.projs[i]); s.i(gs.grenades.projs[i]); }
    s.i(gs.rockets.numprojs); s.i(gs.grenades.numprojs);
    s.i(gs.frags); s.i(gs.flags); s.i(gs.deaths); s.i(gs.teamkills); s.i(gs.shotdamage); s.i(gs.damage); s.i(gs.tokens);
    s.i(gs.lasttimeplayed); s.i(gs.timeplayed); s.f(gs.effectiveness);
}

static void snapevents(snapio &s, vector<gameevent *> &evs)
{
    int n = s.count(evs.length(), 1<<16);
    if(s.loading)
    {
        evs.deletecontents();
        loopi(n)
        {
            int type = 0;
            s.i(type);
            gameevent *e = newsnapevent(type);
            if(!e) { s.ok = false; return; }
            evs.add(e)->snap(s);
        }
        return;
    }
    loopv(evs)
    {
        int type = evs[i]->snaptype();
        s.i(type);
        evs[i]->snap(s);
    }
}

// The clients (and later the bots) must already match the saved ones by client number.
static void snapaiman(snapio &s);

void snapstate(snapio &s)
{
    snapaiman(s);
    s.n(notgotitems); s.i(gamemillis); s.i(gamelimit); s.i(nextexceeded); s.i(gamespeed);
    s.n(gamepaused); s.n(shouldstep); s.i(interm); s.n(flagrunon); s.n(raceruleson);

    // in coop edit the item list only arrives from the client after the map has loaded
    int n = s.count(sents.length(), MAXENTS);
    if(s.loading) { sents.setsize(0); loopi(n) sents.add(); }
    loopv(sents) { s.i(sents[i].type); s.i(sents[i].spawntime); s.n(sents[i].spawned); }

    vector<teaminfo *> teams;
    enumerate(teaminfos, teaminfo, t, teams.add(&t));
    teams.sort(teamnameless);
    n = s.count(teams.length(), MAXTEAMS);
    if(s.loading) teaminfos.clear();
    loopi(n)
    {
        string team;
        int frags = 0;
        if(!s.loading) { copystring(team, teams[i]->team); frags = teams[i]->frags; }
        s.str(team, MAXTEAMLEN+1); s.i(frags);
        if(s.loading && s.ok) { teaminfo &t = teaminfos[team]; copystring(t.team, team, sizeof(t.team)); t.frags = frags; }
    }

    n = s.count(clients.length(), clients.length());
    if(s.loading && n != clients.length()) { s.ok = false; return; }
    loopv(clients)
    {
        clientinfo &ci = *clients[i];
        int cn = ci.clientnum;
        s.i(cn);
        if(cn != ci.clientnum) { s.ok = false; return; }
        s.str(ci.team, sizeof(ci.team));
        s.n(ci.timesync); s.i(ci.gameoffset); s.i(ci.lastevent); s.i(ci.pushed); s.i(ci.exceeded);
        s.i(ci.aireinit); s.n(ci.gameclip);
        snapgamestate(s, ci.state);
        snapevents(s, ci.events);
        if(!s.ok) return;
    }

    int ctf = smode == &ctfmode ? 1 : 0;
    s.i(ctf);
    if(ctf != (smode == &ctfmode ? 1 : 0)) { s.ok = false; return; }
    if(!ctf) return;
    s.n(ctfmode.notgotflags);
    n = s.count(ctfmode.flags.length(), MAXTEAMS*16);
    if(s.loading) { ctfmode.flags.setsize(0); loopi(n) ctfmode.flags.add(); }
    loopv(ctfmode.flags)
    {
        ctfservmode::flag &f = ctfmode.flags[i];
        s.i(f.id); s.i(f.version); s.i(f.spawnindex); s.v(f.droploc); s.v(f.spawnloc);
        s.i(f.team); s.i(f.droptime); s.i(f.owntime); s.i(f.holdtime);
        s.i(f.owner); s.i(f.dropcount); s.i(f.dropper); s.i(f.invistime);
    }
    s.i(ctfmode.scores[0]); s.i(ctfmode.scores[1]);
}
