extern void hashprojectiles(dethasher &h);
extern void hashshotids(dethasher &h);
extern void hashclientstate(dethasher &h);
extern void hashgameclock(dethasher &h);
extern int dethashdetail;
extern void snapshots(snapio &s);
extern void snapgameclock(snapio &s);
extern void snapclientmodes(snapio &s);
extern void snapmessages(snapio &s);
extern void snapmonsters(snapio &s);
extern void snapmonsterfields(snapio &s);
extern void snapmovables(snapio &s);

// savestate
extern void snapfpsent(snapio &s, fpsent &d);
extern int dynentindex(const physent *d);
extern dynent *dynentat(int i);
extern bool savestatetick();
extern bool takesnapshot(vector<uchar> &buf);
extern bool restorestate(vector<uchar> &buf);
extern void holdafterload();
extern void runstateops();
extern void snapcycletick();
extern bool loadsavedstate();
extern void drawinputtimers(int w, int h);
extern void noteworldedit();
extern void savestatemapstart();
extern uint snapshotdigest();
extern void snapcyclereport();
extern const vector<uchar> &savedstatedata(int &tick);
extern bool restorereplaystart(vector<uchar> &buf);

// dethash
// the first DETHASH_LEGACYGROUPS groups are hashed exactly as in replay versions 1-3
enum { DETHASH_GROUPS = 9, DETHASH_LEGACYGROUPS = 6 };
extern void hashgamestate(uint groups[DETHASH_GROUPS]);
extern void dethashframe();

// fps
extern uint gameseed;
extern void seedgame();

// inputs
enum { INPUT_CONTROLS = 0, INPUT_JUMP, INPUT_ATTACK, INPUT_COMMAND, INPUT_GUI, INPUT_ZOOM, INPUT_SETTING, INPUT_MAPGUI };
extern void queueinput(int type, int arg = 0, const char *cmd = NULL);
extern bool deferinput(int type, int arg = 0, const char *cmd = NULL);
extern bool dethashactive();
extern void inputmapstart(const char *name, int mode);
extern int inputtick();
extern void inputrunfinished(int runtime);
// a run finished (hub.cpp): the lobby hears of it
extern void hubrunfinished(int runtime);
extern const char *formatruntime(int ms);
extern void inputdisconnect();
extern void snaprecorder(snapio &s);
extern bool inputreplayfromstate(bool (*restore)());

// runtimer
extern void runtimermapstart(int mode, bool local);
extern void runtimerendsp();
extern void runtimerflagtaken(fpsent *d, int flag, const vec &loc, float radius);
extern void runtimerflagscored(fpsent *d);
extern void runtimertick();
extern int runtimerticks(bool &running);
extern bool runtimerfinished();
extern bool runtimerfinishedbeforemap();
extern int campaignmap(const char *map, int *length);
extern const char *runlabel(const char *map);
extern const char *sprestartmap(int &mode);
extern const char *hubboardhud();
extern const char *runtimerkindname();
extern bool inputrecordinglive();
extern int inputattemptindex();
extern int inputsplicecount();
extern void ghosttick();
extern void splittick();
enum { FINISH_NONE = 0, FINISH_PB, FINISH_FASTER, FINISH_SLOWER };
extern int splitsfinishresult();
extern void splitentityused(int n, fpsent *d, bool teleport);
extern void rendersplitzones();
extern void drawsplits(int w, int h);
extern void renderghost(bool mainpass);
extern void snapruntimer(snapio &s);
extern void snapspawns(snapio &s);
extern void resetspawns(snapio &s);
extern void snaprespawn(snapio &s);
extern void resetrespawn(snapio &s);
extern void snaplatesettings(snapio &s);
extern void resetlatesettings(snapio &s);
extern void snapmorelatesettings(snapio &s);
extern void resetmorelatesettings(snapio &s);
extern bool pickcustomspawn(fpsent *d, int &forceent);
extern void spawnmapstart(const char *map);
extern void respawn();
extern void spawntick();
extern void checkarmedspawn();
extern bool spawncountdown(char *buf);
extern bool runtimerperlife();
extern bool runtimerliferan();
extern int runtimerstarttick();
extern void inputlifespawned();
extern void runtimerspawned(fpsent *d);
extern void runtimeredittoggled(bool on);
extern void drawruntimer(int w, int h);
extern void drawspeedometer(int w, int h);
// hudedit: an element drawn at <name>x, <name>y with <name>scale reports the area it covered, in
// the HUD's unscaled coordinates
extern bool hudediting;
extern void hudeditarea(const char *name, float x0, float y0, float x1, float y1);
extern void drawhudedit(int w, int h);
extern bool flagheldby(int flag, fpsent *d);
extern bool flagspawnloc(bool own, vec &loc);
extern bool flagrunrules();
extern bool racerules();
extern bool mapchained(int mode);
