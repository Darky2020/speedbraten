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
extern bool minimized;
// a key, mouse or focus event as SDL delivered it, before the game handles it
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
