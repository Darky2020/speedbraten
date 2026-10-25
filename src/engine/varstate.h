// Savestates: what map scripts changed since map load, i.e. the overridden variables and
// aliases and the pending map sleeps, in a little endian byte format.
static void sputint(vector<char> &b, int v) { loopk(4) b.add(char(uint(v) >> (k*8))); }
static void sputfloat(vector<char> &b, float f) { int v; memcpy(&v, &f, sizeof(v)); sputint(b, v); }
static void sputstr(vector<char> &b, const char *s) { int n = strlen(s); sputint(b, n); b.put(s, n); }

struct scriptreader
{
    const vector<char> &b;
    int p;
    bool ok;

    scriptreader(const vector<char> &buf) : b(buf), p(0), ok(true) {}
    int getint()
    {
        if(p + 4 > b.length()) { ok = false; return 0; }
        uint v = 0;
        loopk(4) v |= uint(uchar(b[p++])) << (k*8);
        return int(v);
    }
    float getfloat() { int v = getint(); float f; memcpy(&f, &v, sizeof(f)); return f; }
    char *getstr()
    {
        int n = getint();
        if(n < 0 || p + n > b.length()) { ok = false; return newstring(""); }
        char *s = newstring(&b[p], n);
        p += n;
        return s;
    }
};

struct savedident
{
    char *name, *s;
    int type, valtype, i;
    float f;

    savedident() : name(NULL), s(NULL), type(0), valtype(VAL_NULL), i(0), f(0) {}
    ~savedident() { DELETEA(name); DELETEA(s); }
};

static bool identnameless(ident *a, ident *b) { return strcmp(a->name, b->name) < 0; }

static bool savedmatches(const ident &id, const savedident &v)
{
    if(!(id.flags&IDF_OVERRIDDEN) || id.type != v.type) return false;
    switch(id.type)
    {
        case ID_VAR: return *id.storage.i == v.i;
        case ID_FVAR: return !memcmp(id.storage.f, &v.f, sizeof(float));
        case ID_SVAR: return !strcmp(*id.storage.s, v.s);
        case ID_ALIAS:
            if(id.valtype != v.valtype) return false;
            switch(id.valtype)
            {
                case VAL_INT: return id.val.i == v.i;
                case VAL_FLOAT: return !memcmp(&id.val.f, &v.f, sizeof(float));
                case VAL_STR: return !strcmp(id.val.s, v.s);
                default: return true;
            }
    }
    return false;
}

void savescriptstate(vector<char> &b)
{
    vector<ident *> ids;
    enumerate(idents, ident, id,
        if(id.flags&IDF_OVERRIDDEN && (id.type == ID_VAR || id.type == ID_FVAR || id.type == ID_SVAR || id.type == ID_ALIAS)) ids.add(&id));
    ids.sort(identnameless);
    sputint(b, ids.length());
    loopv(ids)
    {
        const ident &id = *ids[i];
        sputstr(b, id.name);
        sputint(b, id.type);
        switch(id.type)
        {
            case ID_VAR: sputint(b, *id.storage.i); break;
            case ID_FVAR: sputfloat(b, *id.storage.f); break;
            case ID_SVAR: sputstr(b, *id.storage.s); break;
            case ID_ALIAS:
                switch(id.valtype)
                {
                    case VAL_INT: sputint(b, VAL_INT); sputint(b, id.val.i); break;
                    case VAL_FLOAT: sputint(b, VAL_FLOAT); sputfloat(b, id.val.f); break;
                    case VAL_STR: sputint(b, VAL_STR); sputstr(b, id.val.s); break;
                    default: sputint(b, VAL_NULL); break;
                }
                break;
        }
    }
    int n = 0;
    loopv(sleepcmds) if(sleepcmds[i].command && sleepcmds[i].flags&IDF_OVERRIDDEN) n++;
    sputint(b, n);
    loopv(sleepcmds)
    {
        const sleepcmd &s = sleepcmds[i];
        if(!s.command || !(s.flags&IDF_OVERRIDDEN)) continue;
        sputint(b, s.delay); sputint(b, s.millis); sputint(b, s.flags); sputstr(b, s.command);
    }
}

// Only idents whose value or override state differs are touched, so variable callbacks run
// only for real changes.
bool loadscriptstate(const vector<char> &b)
{
    scriptreader r(b);
    int n = r.getint();
    if(n < 0 || n > (1<<20)) return false;
    vector<savedident *> saved;
    loopi(n)
    {
        if(!r.ok) break;
        savedident &v = *saved.add(new savedident);
        v.name = r.getstr();
        v.type = r.getint();
        switch(v.type)
        {
            case ID_VAR: v.i = r.getint(); break;
            case ID_FVAR: v.f = r.getfloat(); break;
            case ID_SVAR: v.s = r.getstr(); break;
            case ID_ALIAS:
                v.valtype = r.getint();
                switch(v.valtype)
                {
                    case VAL_INT: v.i = r.getint(); break;
                    case VAL_FLOAT: v.f = r.getfloat(); break;
                    case VAL_STR: v.s = r.getstr(); break;
                    case VAL_NULL: break;
                    default: r.ok = false; break;
                }
                break;
            default: r.ok = false; break;
        }
    }
    int numsleeps = r.ok ? r.getint() : 0;
    if(numsleeps < 0 || numsleeps > (1<<20)) r.ok = false;
    vector<sleepcmd> sleeps;
    loopi(numsleeps)
    {
        if(!r.ok) break;
        sleepcmd &s = sleeps.add();
        s.delay = r.getint(); s.millis = r.getint(); s.flags = r.getint(); s.command = r.getstr();
    }
    if(!r.ok || r.p != b.length())
    {
        saved.deletecontents();
        loopv(sleeps) delete[] sleeps[i].command;
        return false;
    }

    hashtable<const char *, int> keep;
    loopv(saved) keep[saved[i]->name] = i;
    enumerate(idents, ident, id, if(id.flags&IDF_OVERRIDDEN && !keep.access(id.name)) clearoverride(id));
    int oldflags = identflags;
    identflags |= IDF_OVERRIDDEN;
    loopv(saved)
    {
        const savedident &v = *saved[i];
        ident *id = idents.access(v.name);
        if(id && savedmatches(*id, v)) continue;
        if(id && id->type != v.type) continue;
        switch(v.type)
        {
            case ID_VAR: setvar(v.name, v.i, true, false); break;
            case ID_FVAR: setfvar(v.name, v.f, true, false); break;
            case ID_SVAR: setsvar(v.name, v.s, true); break;
            case ID_ALIAS:
            {
                tagval t;
                switch(v.valtype)
                {
                    case VAL_INT: t.setint(v.i); break;
                    case VAL_FLOAT: t.setfloat(v.f); break;
                    case VAL_STR: t.setstr(newstring(v.s)); break;
                    default: t.setnull(); break;
                }
                setalias(v.name, t);
                break;
            }
        }
    }
    identflags = oldflags;
    saved.deletecontents();

    int len = 0;
    loopv(sleepcmds)
    {
        if(sleepcmds[i].command && !(sleepcmds[i].flags&IDF_OVERRIDDEN)) sleepcmds[len++] = sleepcmds[i];
        else delete[] sleepcmds[i].command;
    }
    sleepcmds.shrink(len);
    loopv(sleeps) sleepcmds.add(sleeps[i]);
    return true;
}
