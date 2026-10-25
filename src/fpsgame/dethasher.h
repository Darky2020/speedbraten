struct dethasher
{
    uint h;
    dethasher() : h(2166136261U) {}
    void u(uint v) { h = (h ^ v) * 16777619U; }
    void i(int v) { u(uint(v)); }
    // NaN sign and payload bits differ between x86 and ARM
    void f(float v) { uint b; memcpy(&b, &v, sizeof(b)); u(v == v ? b : 0x7FC00000U); }
    void v(const vec &p) { f(p.x); f(p.y); f(p.z); }
    void phys(const physent *d)
    {
        v(d->o); v(d->newpos); v(d->vel); v(d->falling); v(d->floor);
        f(d->yaw); f(d->pitch);
        i(d->timeinair); i(d->inwater); i(d->jumping); i(d->move); i(d->strafe);
        i(d->physstate); i(d->state); i(d->blocked);
    }
};

// Savestate stream. One function both saves and restores a piece of state (loading says
// which), so the field order cannot differ between the two. Integers are little endian and
// floats keep their exact bits.
struct snapio
{
    vector<uchar> &buf;
    bool loading, ok;
    int pos;

    snapio(vector<uchar> &b, bool load) : buf(b), loading(load), ok(true), pos(0) {}

    void u(uint &v)
    {
        if(!loading) { loopk(4) buf.add(uchar(v >> (k*8))); return; }
        if(pos + 4 > buf.length()) { ok = false; v = 0; return; }
        v = 0;
        loopk(4) v |= uint(buf[pos++]) << (k*8);
    }
    void i(int &v) { uint x = uint(v); u(x); v = int(x); }
    template<class T> void n(T &v) { int x = int(v); i(x); if(loading) v = T(x); }
    void f(float &v) { uint x; memcpy(&x, &v, sizeof(x)); u(x); memcpy(&v, &x, sizeof(x)); }
    void v(vec &p) { f(p.x); f(p.y); f(p.z); }
    // a stored count; on load it must lie in [0, limit]
    int count(int n, int limit)
    {
        i(n);
        if(n < 0 || n > limit) { ok = false; return 0; }
        return n;
    }
    void bytes(vector<char> &b)
    {
        int len = count(b.length(), 1<<20);
        if(!loading) { loopk(len) buf.add(uchar(b[k])); return; }
        b.setsize(0);
        if(!ok || pos + len > buf.length()) { ok = false; return; }
        loopk(len) b.add(char(buf[pos++]));
    }
    void str(char *s, int maxlen)
    {
        int len = count(loading ? 0 : int(strlen(s)), maxlen-1);
        if(!loading) { loopk(len) buf.add(uchar(s[k])); return; }
        if(!ok || pos + len > buf.length()) { ok = false; s[0] = '\0'; return; }
        memcpy(s, &buf[pos], len);
        s[len] = '\0';
        pos += len;
    }
    void tag(uint t) { uint x = t; u(x); if(x != t) ok = false; }
    // loading: whether the next word is tag t, without reading it
    bool peektag(uint t) const
    {
        if(pos + 4 > buf.length()) return false;
        uint v = 0;
        loopk(4) v |= uint(buf[pos+k]) << (k*8);
        return v == t;
    }
    void phys(physent &d)
    {
        v(d.o); v(d.vel); v(d.falling); v(d.deltapos); v(d.newpos); i(d.interpmillis);
        f(d.yaw); f(d.pitch); f(d.roll); f(d.maxspeed);
        f(d.radius); f(d.eyeheight); f(d.aboveeye); f(d.xradius); f(d.yradius); f(d.zmargin);
        v(d.floor);
        n(d.timeinair); n(d.inwater); n(d.jumping); n(d.hovering); n(d.move); n(d.strafe); n(d.vertical);
        n(d.physstate); n(d.state); n(d.editstate); n(d.type); n(d.collidetype); n(d.blocked);
    }
    void state(fpsstate &s)
    {
        i(s.health); i(s.maxhealth); i(s.armour); i(s.armourtype); i(s.quadmillis);
        i(s.gunselect); i(s.gunwait);
        loopk(NUMGUNS) i(s.ammo[k]);
        i(s.aitype); i(s.skill);
    }
};
