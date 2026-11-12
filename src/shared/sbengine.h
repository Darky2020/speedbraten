extern void g3d_setcursor(float x, float y);
extern void schedulemenuaction(const char *action, bool scripted);
extern bool getmapguistate(vector<char> &names, float &cx, float &cy);
extern void setmapguistate(const char *names, float cx, float cy);
extern bool mapguiontop();

// where the command running now came from, for tagging recorded input
// INPUTSRC_RECORDER: found by the recorder (controls, GUI state, a setting the player changed);
// INPUTSRC_MAP: a setting the map changed, while loading or from its script during a tick
enum { INPUTSRC_OTHER = 0, INPUTSRC_KEY, INPUTSRC_MOUSE, INPUTSRC_CONSOLE, INPUTSRC_MENU, INPUTSRC_HARNESS, INPUTSRC_RECORDER, INPUTSRC_MAP };
extern int inputsource;
extern int fixedframe;
extern int screenw, screenh;
extern int practicevisualizers;
extern bool minimized;
// a key, mouse or focus event as SDL delivered it, before the game handles it
extern void simfastforward(int ms);
extern int simspeedpercent;
// a movie on its own clock: each frame advances the game by exactly one video frame and none is dropped
// hold: the clock runs on video frames already, but nothing is captured until movierenderhold(false)
extern bool movierender(const char *name, int fps, int w, int h, bool hold = false);
extern void movierenderhold(bool on);
// a bar, label and hint line over the window (not the movie) while a render runs; frac < 0: no bar;
// a NULL label hides it
extern void movieprogress(const char *label, float frac, const char *hint = NULL);
// the last rendered movie's name; moviediscard deletes it
extern const char *movielastname();
extern void moviediscard();
// a real-time pass that records the mixer's output and, on stop with mux, adds it to the last rendered movie
extern bool moviesoundstart();
extern bool moviesoundrecording();
// marks the recording where a sound started now would be mixed in; moviesoundmarkms: that place, -1 until then
extern void moviesoundmark();
extern int moviesoundmarkms();
// mux: put together from n pieces of the recording, piece i from startms[i] (negative: that much
// silence first) for lengthms[i] (the last: to the end), one piece per map of the movie
extern void moviesoundstop(bool mux, const int *startms, const int *lengthms, int n);
extern void movierenderstop();
// while a movie renders off-screen: the HUD's text resolution, as tall as the window's, so the HUD
// keeps the window's proportions at any movie size
extern void moviehudres(int &w, int &h);
extern bool movierendering();
extern int movierenderstep();
extern uint movierenderframes();
// set while a map's script runs: its binds last until restoremapbinds, at the next map load
// console lines only the full console shows (left out of the top left one and the mini console)
enum { CON_QUIET = 1<<15 };
extern bool mapbinding;
extern void restoremapbinds();

struct rawevent
{
    enum { KEYDOWN = 0, KEYUP, BUTTONDOWN, BUTTONUP, MOTION, WHEEL, FOCUS };
    enum { REPEAT = 1<<0, GRABBED = 1<<1, GAINED = 1<<2, TEXT = 1<<3 }; // TEXT: typed into a text field, key left out
    int tick, kind;
    long long us;      // since the run began, when the game read the event
    uint sdlms;        // SDL's event timestamp
    int code, keycode; // key: scancode and keycode; button: SDL button
    int dx, dy;        // motion, wheel
    int flags;
    const char *bind;  // what the key or button is bound to now, NULL if nothing

    rawevent(int kind = KEYDOWN, uint sdlms = 0) : tick(0), kind(kind), us(0), sdlms(sdlms), code(0), keycode(0), dx(0), dy(0), flags(0), bind(NULL) {}
};
extern const char *getbindaction(int code);

// the encrypted channel to the witness (engine/witnessnet.cpp); messages in shared/witnessproto.h
enum { WITNESS_IDLE = 0, WITNESS_CONNECTING, WITNESS_OPEN, WITNESS_CLOSED };
// an empty host closes the channel; serverkey is the witness's pinned X25519 public key
extern void witnessconnect(const char *host, int port, const uchar *serverkey, const char *build);
extern int witnessstate();
extern const char *witnesserror();
extern bool witnesslearnedkeys(uchar *channelkey, uchar *signingkey);
extern bool witnesssend(int type, const uchar *data, int len);
// the next received message; type -1 is the witness's handshake payload
extern bool witnessreceive(int &type, vector<uchar> &payload);
extern void witnessslice();
// the same channel to the hub (messages in shared/hubproto.h); witnessslice moves both along
extern void hubconnect(const char *host, int port, const uchar *serverkey, const char *build);
extern int hubstate();
extern const char *huberror();
extern bool hublearnedkey(uchar *channelkey);
extern bool hubsend(int type, const uchar *data, int len);
// bytes waiting to go out to the hub; hubsend fails the connection past a few MB
extern int hubqueued();

// Top edges that a body rising alongside their side can "land" on for a moment, which clears the
// gravity built up since it left the ground (the practice visualizers' bounce edges): a floor-like
// top face, a near-vertical side right under the edge, the floor in front of the side height units
// lower. approaches: a bit per clear approach, 1<<(2*angle + end) with angle 0..3 for
// BOUNCEANGLES and end 0/1 for coming in towards b or towards a; keep: the share of horizontal speed
// left after sliding into the side at the shallowest clear one, 0 when none is clear. out points
// away from the side. Found again when the map changes.
enum { NUMBOUNCEANGLES = 4 };
extern const float BOUNCEANGLES[NUMBOUNCEANGLES];
struct bounceedge { vec a, b, out; float height, keep; int approaches; };
// the takeoff point (on the floor) and where the body touches the side, of one approach
extern void bounceapproachpath(const bounceedge &e, int angle, int end, vec &takeoff, vec &contact);
// a standing jump at an edge head on, without touching the side: seconds until the feet are at the
// top (reach) and STAIRHEIGHT below it (stair), and how long they stay in the 0.1 units above the
// top where crossing the edge counts as a landing (window); false if it doesn't reach the top
extern bool bounceflyover(float height, float &reach, float &stair, float &window);
// how far a jump taken at horizontal speed (running forward) travels in secs: on its first tick the
// speed still heads for the ground's top speed, after that for the faster one in the air
extern float bouncerunup(float speed, float secs);
// where the player's feet were when their last jump left the ground, the horizontal speed then and
// lastmillis at the time (-1 before the first)
// the player's last mid-air gravity reset (a bump), as the physics found it: BUMP_LANDING when a floor
// was found under the edge of the body while rising, BUMP_STEP for a step up onto an edge from a
// fall, BUMP_STEPJUMP when a jump followed that step; millis -1 before the first. entry and after:
// horizontal speed going in and a tick later; upbefore and upafter: upward speed (velocity plus
// built-up gravity) before and after; gravity: the built-up gravity it cleared.
enum { BUMP_LANDING = 0, BUMP_STEP, BUMP_STEPJUMP };
// entryvel: horizontal velocity going in; wall: the normal of the wall first touched since leaving the
// ground, zero without one.
struct bumpinfo { int kind, millis; vec o, entryvel, wall; float entry, after, upbefore, upafter, gravity; };
extern bumpinfo lastbump;
extern vec lastjumpat;
extern float lastjumpspeed;
extern int lastjumpmillis;
// how far a ray goes before something a player collides with: geometry, except where noclip makes
// it empty, and clip material cubes; other materials are passed through. maxdist when nothing is hit.
extern float bounceray(const vec &o, const vec &dir, float maxdist);
// speed: the run's horizontal speed the approaches are checked for (searched again when it changes)
extern const vector<bounceedge> &bounceedges(float speed);
// the upward speed such an edge gives after a standing jump from the floor height units below its
// top, -1 when a standing jump doesn't reach it
extern float bouncegain(float height);
extern bool hubreceive(int &type, vector<uchar> &payload);

// an input device the system reports; kind: 0 other, 1 keyboard, 2 mouse
struct inputdevice
{
    int kind, vendor, product;
    string transport, name; // UTF-8
    inputdevice() : kind(0), vendor(0), product(0) { transport[0] = name[0] = '\0'; }
};
extern void listinputdevices(vector<inputdevice> &out);
// a coarse description of the OS and the CPU model, MAXSTRLEN each
extern void systeminfo(char *os, char *cpu);

struct inputsourcescope
{
    int old;
    inputsourcescope(int src) : old(inputsource) { inputsource = src; }
    ~inputsourcescope() { inputsource = old; }
};

// untextured rectangles in the HUD's current coordinates (engine/huddraw.cpp)
extern void hudfillrect(float x0, float y0, float x1, float y1, int colour, int alpha = 255);
extern void hudoutlinerect(float x0, float y0, float x1, float y1, float width, int colour, int alpha = 255);
extern void hudfilldisc(float cx, float cy, float r, int colour, int alpha = 255);
