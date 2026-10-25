void hashclientstate(dethasher &h)
{
    // The outgoing message queue is left out: map-start messages are queued at different
    // points of the first tick depending on how the map was loaded, and what reaches the
    // local server shows up in its own hash group.
    if(dethashdetail) conoutf("dethashdetail client: messages %d cn %d reliable %d senditems %d sendcrc %d", messages.length(), messagecn, messagereliable, senditemstoserver, sendcrc);
    if(m_ctf)
    {
        loopv(ctfmode.flags)
        {
            const ctfclientmode::flag &f = ctfmode.flags[i];
            // droploc is only read while the flag is dropped, and is left stale otherwise
            h.i(f.id); h.i(f.version); h.i(f.spawnindex); if(f.droptime) h.v(f.droploc); h.v(f.spawnloc);
            h.i(f.team); h.i(f.droptime); h.i(f.owntime); h.i(f.owner ? f.owner->clientnum : -1);
            if(dethashdetail) conoutf("dethashdetail flag %d: version %d spawnindex %d team %d droptime %d owntime %d owner %d droploc %.3f %.3f %.3f spawnloc %.3f %.3f %.3f", f.id, f.version, f.spawnindex, f.team, f.droptime, f.owntime, f.owner ? f.owner->clientnum : -1, f.droploc.x, f.droploc.y, f.droploc.z, f.spawnloc.x, f.spawnloc.y, f.spawnloc.z);
        }
        h.i(ctfmode.scores[0]); h.i(ctfmode.scores[1]);
    }
}

// Messages queued after the tick's c2sinfo, such as the reply to a spawn, wait in the queue
// until the next tick.
void snapmessages(snapio &s)
{
    s.n(senditemstoserver); s.n(sendcrc);
    int n = s.count(messages.length(), 1<<20);
    if(s.loading) { messages.setsize(0); loopi(n) messages.add(0); }
    loopv(messages) s.n(messages[i]);
    s.i(messagecn); s.i(messagereliable);
}

// The parts of the flag state that matter to play: a fresh life must have the same flags
// as a fresh map start (scores, reset counters, timestamps and drawing state aside).
uint lifeflagsdigest()
{
    if(!m_ctf) return 0;
    dethasher h;
    loopv(ctfmode.flags)
    {
        ctfclientmode::flag &f = ctfmode.flags[i];
        h.i(f.id); h.i(f.team); h.i(f.spawnindex); h.v(f.spawnloc); h.f(f.spawnangle);
        h.i(f.owner ? f.owner->clientnum : -1);
        h.i(f.droptime ? 1 : 0);
        if(f.droptime) { h.v(f.droploc); h.f(f.dropangle); }
    }
    return h.h;
}

// The messages queued for the server, split into messages (type and arguments, fixed
// sizes only); false if the queue holds one of variable size.
static bool splitmessages(const vector<uchar> &buf, vector<vector<int> > &out)
{
    ucharbuf p((uchar *)buf.getbuf(), buf.length());
    while(p.remaining())
    {
        int type = getint(p), size = server::msgsizelookup(type);
        if(size <= 0 || p.overread()) return false;
        vector<int> &m = out.add();
        m.add(type);
        loopi(size-1) m.add(getint(p));
        if(p.overread()) return false;
    }
    return true;
}

static vector<uchar> freshmessages;
void lifemessagesfresh() { freshmessages = messages; }

// A life starts with its own spawn queued for the server and otherwise only messages a
// fresh map start also has queued (a life can begin at the map start), besides player
// model changes.
bool lifemessagesok()
{
    vector<vector<int> > life, fresh;
    if(!splitmessages(messages, life) || !splitmessages(freshmessages, fresh)) return false;
    int spawns = 0;
    loopv(life)
    {
        const vector<int> &m = life[i];
        if(m[0] == N_SPAWN)
        {
            if(m.length() != 3 || m[1] != player1->lifesequence || m[2] != player1->gunselect || spawns++) return false;
            continue;
        }
        if(m[0] == N_SWITCHMODEL) continue;
        int found = -1;
        loopvj(fresh) if(fresh[j].length() == m.length() && !memcmp(fresh[j].getbuf(), m.getbuf(), m.length()*sizeof(int))) { found = j; break; }
        if(found < 0) return false;
        fresh.remove(found);
    }
    return spawns == 1;
}

void snapclientmodes(snapio &s)
{
    int ctf = m_ctf ? 1 : 0;
    s.i(ctf);
    if(ctf != (m_ctf ? 1 : 0)) { s.ok = false; return; }
    if(!ctf) return;
    int n = s.count(ctfmode.flags.length(), MAXTEAMS*16);
    if(s.loading) { ctfmode.flags.setsize(0); loopi(n) ctfmode.flags.add(); }
    loopv(ctfmode.flags)
    {
        ctfclientmode::flag &f = ctfmode.flags[i];
        s.i(f.id); s.i(f.version); s.i(f.spawnindex); s.v(f.droploc); s.v(f.spawnloc);
        s.i(f.team); s.i(f.droptime); s.i(f.owntime); s.i(f.holdtime);
        int owner = s.loading ? -1 : dynentindex(f.owner);
        s.i(owner);
        if(s.loading) f.owner = (fpsent *)dynentat(owner);
        s.f(f.dropangle); s.f(f.spawnangle); s.v(f.interploc); s.f(f.interpangle); s.i(f.interptime); s.i(f.vistime);
    }
    s.i(ctfmode.scores[0]); s.i(ctfmode.scores[1]);
}
