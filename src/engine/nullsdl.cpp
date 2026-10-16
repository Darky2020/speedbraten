// SDL, SDL_mixer and SDL_image for the headless re-simulation build (sauer_resim), which
// links none of them. There is no window, input, audio or image decoding, so those calls
// fail or report "nothing there". Clock, threads, mutexes, surfaces and RWops are real,
// minimal implementations of what the engine relies on.

#ifdef HEADLESS

#include "engine.h"
#ifdef __APPLE__
  #include <SDL2_mixer/SDL_mixer.h>
  #include <SDL2_image/SDL_image.h>
#else
  #include "SDL_mixer.h"
  #include "SDL_image.h"
#endif

#ifdef WIN32
  #include <windows.h>
#else
  #include <errno.h>
  #include <pthread.h>
  #include <time.h>
  #include <unistd.h>
#endif


#ifdef WIN32
struct SDL_mutex { CRITICAL_SECTION cs; };
struct SDL_cond { CONDITION_VARIABLE cv; };
struct SDL_Thread { HANDLE h; DWORD id; SDL_ThreadFunction fn; void *data; int status; };
static DWORD WINAPI threadmain(LPVOID arg) { SDL_Thread *t = (SDL_Thread *)arg; t->status = t->fn(t->data); return 0; }
#else
struct SDL_mutex { pthread_mutex_t m; };
struct SDL_cond { pthread_cond_t c; };
struct SDL_Thread { pthread_t t; SDL_ThreadFunction fn; void *data; int status; };
static void *threadmain(void *arg) { SDL_Thread *t = (SDL_Thread *)arg; t->status = t->fn(t->data); return NULL; }
#endif

static Uint32 masksize(Uint32 mask) { Uint32 n = 0; for(; mask; mask >>= 1) n += mask&1; return n; }
static Uint8 maskshift(Uint32 mask) { Uint8 n = 0; if(mask) while(!(mask&1)) { mask >>= 1; n++; } return n; }

static SDL_Surface *newsurface(int w, int h, int depth, int pitch, void *pixels, Uint32 r, Uint32 g, Uint32 b, Uint32 a)
{
    if(w <= 0 || h <= 0 || (depth != 8 && depth != 16 && depth != 24 && depth != 32)) return NULL;
    SDL_Surface *s = (SDL_Surface *)calloc(1, sizeof(SDL_Surface));
    SDL_PixelFormat *f = (SDL_PixelFormat *)calloc(1, sizeof(SDL_PixelFormat));
    f->BitsPerPixel = depth;
    f->BytesPerPixel = depth/8;
    f->Rmask = r; f->Gmask = g; f->Bmask = b; f->Amask = a;
    f->Rshift = maskshift(r); f->Gshift = maskshift(g); f->Bshift = maskshift(b); f->Ashift = maskshift(a);
    f->Rloss = 8 - masksize(r); f->Gloss = 8 - masksize(g); f->Bloss = 8 - masksize(b); f->Aloss = 8 - masksize(a);
    f->refcount = 1;
    s->format = f;
    s->w = w;
    s->h = h;
    s->refcount = 1;
    s->clip_rect.w = w;
    s->clip_rect.h = h;
    if(pixels) { s->pixels = pixels; s->pitch = pitch; s->flags = SDL_PREALLOC; }
    else { s->pitch = (w*f->BytesPerPixel + 3)&~3; s->pixels = calloc(1, size_t(s->pitch)*h); }
    return s;
}

static Uint32 getpixel(const SDL_Surface *s, int x, int y)
{
    const Uint8 *p = (const Uint8 *)s->pixels + y*s->pitch + x*s->format->BytesPerPixel;
    Uint32 v = 0;
    memcpy(&v, p, s->format->BytesPerPixel);
    return v;
}

static void putpixel(SDL_Surface *s, int x, int y, Uint32 v)
{
    memcpy((Uint8 *)s->pixels + y*s->pitch + x*s->format->BytesPerPixel, &v, s->format->BytesPerPixel);
}

static Uint32 convertchannel(Uint32 v, Uint32 srcmask, Uint32 dstmask, Uint32 dflt)
{
    if(!dstmask) return 0;
    Uint32 c = srcmask ? ((v & srcmask) >> maskshift(srcmask)) << (8 - masksize(srcmask)) : dflt;
    return ((c >> (8 - masksize(dstmask))) << maskshift(dstmask)) & dstmask;
}

extern "C" {

void SDLCALL SDL_SetMainReady(void) {}
int SDLCALL SDL_Init(Uint32 flags) { return 0; }
int SDLCALL SDL_InitSubSystem(Uint32 flags) { return 0; }
void SDLCALL SDL_QuitSubSystem(Uint32 flags) {}
void SDLCALL SDL_Quit(void) {}
Uint32 SDLCALL SDL_WasInit(Uint32 flags) { return 0; }
const char *SDLCALL SDL_GetError(void) { return ""; }
void SDLCALL SDL_GetVersion(SDL_version *ver) { ver->major = SDL_MAJOR_VERSION; ver->minor = SDL_MINOR_VERSION; ver->patch = SDL_PATCHLEVEL; }
SDL_bool SDLCALL SDL_SetHint(const char *name, const char *value) { return SDL_TRUE; }
int SDLCALL SDL_GetCPUCount(void)
{
#ifdef WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return max(int(info.dwNumberOfProcessors), 1);
#else
    return max(int(sysconf(_SC_NPROCESSORS_ONLN)), 1);
#endif
}

void *SDLCALL SDL_malloc(size_t size) { return malloc(size); }
void SDLCALL SDL_free(void *mem) { free(mem); }
void *SDLCALL SDL_memcpy(void *dst, const void *src, size_t len) { return memcpy(dst, src, len); }
char *SDLCALL SDL_getenv(const char *name) { return getenv(name); }
int SDLCALL SDL_setenv(const char *name, const char *value, int overwrite)
{
    if(!overwrite && getenv(name)) return 0;
#ifdef WIN32
    return _putenv_s(name, value);
#else
    return setenv(name, value, 1);
#endif
}

Uint32 SDLCALL SDL_GetTicks(void)
{
#ifdef WIN32
    static ULONGLONG start = GetTickCount64();
    return Uint32(GetTickCount64() - start);
#else
    static timespec start = { 0, 0 };
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if(!start.tv_sec && !start.tv_nsec) start = now;
    return Uint32((now.tv_sec - start.tv_sec)*1000 + (now.tv_nsec - start.tv_nsec)/1000000);
#endif
}
void SDLCALL SDL_Delay(Uint32 ms)
{
#ifdef WIN32
    Sleep(ms);
#else
    timespec t = { time_t(ms/1000), long(ms%1000)*1000000 };
    nanosleep(&t, NULL);
#endif
}
SDL_TimerID SDLCALL SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback, void *param) { return 0; }
SDL_bool SDLCALL SDL_RemoveTimer(SDL_TimerID id) { return SDL_FALSE; }

#ifdef SDL_PASSED_BEGINTHREAD_ENDTHREAD
SDL_Thread *SDLCALL (SDL_CreateThread)(SDL_ThreadFunction fn, const char *name, void *data, pfnSDL_CurrentBeginThread pfnBeginThread, pfnSDL_CurrentEndThread pfnEndThread)
#else
SDL_Thread *SDLCALL (SDL_CreateThread)(SDL_ThreadFunction fn, const char *name, void *data)
#endif
{
    SDL_Thread *t = (SDL_Thread *)calloc(1, sizeof(SDL_Thread));
    t->fn = fn;
    t->data = data;
#ifdef WIN32
    t->h = CreateThread(NULL, 0, threadmain, t, 0, &t->id);
    if(!t->h) { free(t); return NULL; }
#else
    if(pthread_create(&t->t, NULL, threadmain, t)) { free(t); return NULL; }
#endif
    return t;
}
void SDLCALL SDL_WaitThread(SDL_Thread *thread, int *status)
{
    if(!thread) return;
#ifdef WIN32
    WaitForSingleObject(thread->h, INFINITE);
    CloseHandle(thread->h);
#else
    pthread_join(thread->t, NULL);
#endif
    if(status) *status = thread->status;
    free(thread);
}
void SDLCALL SDL_DetachThread(SDL_Thread *thread)
{
    if(!thread) return;
#ifdef WIN32
    CloseHandle(thread->h);
#else
    pthread_detach(thread->t);
#endif
}
#ifdef WIN32
SDL_threadID SDLCALL SDL_ThreadID(void) { return SDL_threadID(GetCurrentThreadId()); }
SDL_threadID SDLCALL SDL_GetThreadID(SDL_Thread *thread) { return thread ? SDL_threadID(thread->id) : SDL_ThreadID(); }

SDL_mutex *SDLCALL SDL_CreateMutex(void) { SDL_mutex *m = (SDL_mutex *)malloc(sizeof(SDL_mutex)); InitializeCriticalSection(&m->cs); return m; }
void SDLCALL SDL_DestroyMutex(SDL_mutex *mutex) { if(mutex) { DeleteCriticalSection(&mutex->cs); free(mutex); } }
int SDLCALL SDL_LockMutex(SDL_mutex *mutex) { if(!mutex) return -1; EnterCriticalSection(&mutex->cs); return 0; }
int SDLCALL SDL_UnlockMutex(SDL_mutex *mutex) { if(!mutex) return -1; LeaveCriticalSection(&mutex->cs); return 0; }
SDL_cond *SDLCALL SDL_CreateCond(void) { SDL_cond *c = (SDL_cond *)malloc(sizeof(SDL_cond)); InitializeConditionVariable(&c->cv); return c; }
void SDLCALL SDL_DestroyCond(SDL_cond *cond) { free(cond); }
int SDLCALL SDL_CondSignal(SDL_cond *cond) { if(!cond) return -1; WakeConditionVariable(&cond->cv); return 0; }
int SDLCALL SDL_CondBroadcast(SDL_cond *cond) { if(!cond) return -1; WakeAllConditionVariable(&cond->cv); return 0; }
int SDLCALL SDL_CondWaitTimeout(SDL_cond *cond, SDL_mutex *mutex, Uint32 ms)
{
    if(!cond || !mutex) return -1;
    if(SleepConditionVariableCS(&cond->cv, &mutex->cs, ms)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? SDL_MUTEX_TIMEDOUT : -1;
}
int SDLCALL SDL_CondWait(SDL_cond *cond, SDL_mutex *mutex) { return SDL_CondWaitTimeout(cond, mutex, INFINITE); }
#else
SDL_threadID SDLCALL SDL_ThreadID(void) { return SDL_threadID(size_t(pthread_self())); }
SDL_threadID SDLCALL SDL_GetThreadID(SDL_Thread *thread) { return thread ? SDL_threadID(size_t(thread->t)) : SDL_ThreadID(); }

SDL_mutex *SDLCALL SDL_CreateMutex(void)
{
    SDL_mutex *m = (SDL_mutex *)malloc(sizeof(SDL_mutex));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->m, &attr);
    pthread_mutexattr_destroy(&attr);
    return m;
}
void SDLCALL SDL_DestroyMutex(SDL_mutex *mutex) { if(mutex) { pthread_mutex_destroy(&mutex->m); free(mutex); } }
int SDLCALL SDL_LockMutex(SDL_mutex *mutex) { return mutex ? pthread_mutex_lock(&mutex->m) : -1; }
int SDLCALL SDL_UnlockMutex(SDL_mutex *mutex) { return mutex ? pthread_mutex_unlock(&mutex->m) : -1; }
SDL_cond *SDLCALL SDL_CreateCond(void) { SDL_cond *c = (SDL_cond *)malloc(sizeof(SDL_cond)); pthread_cond_init(&c->c, NULL); return c; }
void SDLCALL SDL_DestroyCond(SDL_cond *cond) { if(cond) { pthread_cond_destroy(&cond->c); free(cond); } }
int SDLCALL SDL_CondSignal(SDL_cond *cond) { return cond ? pthread_cond_signal(&cond->c) : -1; }
int SDLCALL SDL_CondBroadcast(SDL_cond *cond) { return cond ? pthread_cond_broadcast(&cond->c) : -1; }
int SDLCALL SDL_CondWait(SDL_cond *cond, SDL_mutex *mutex) { return cond && mutex ? pthread_cond_wait(&cond->c, &mutex->m) : -1; }
int SDLCALL SDL_CondWaitTimeout(SDL_cond *cond, SDL_mutex *mutex, Uint32 ms)
{
    if(!cond || !mutex) return -1;
    timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms/1000;
    t.tv_nsec += long(ms%1000)*1000000;
    if(t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
    int err = pthread_cond_timedwait(&cond->c, &mutex->m, &t);
    return err == ETIMEDOUT ? SDL_MUTEX_TIMEDOUT : (err ? -1 : 0);
}
#endif

void SDLCALL SDL_PumpEvents(void) {}
int SDLCALL SDL_PeepEvents(SDL_Event *events, int numevents, SDL_eventaction action, Uint32 minType, Uint32 maxType) { return 0; }
void SDLCALL SDL_FlushEvent(Uint32 type) {}
void SDLCALL SDL_FilterEvents(SDL_EventFilter filter, void *userdata) {}
void SDLCALL SDL_AddEventWatch(SDL_EventFilter filter, void *userdata) {}
void SDLCALL SDL_DelEventWatch(SDL_EventFilter filter, void *userdata) {}
SDL_Keymod SDLCALL SDL_GetModState(void) { return KMOD_NONE; }
void SDLCALL SDL_StartTextInput(void) {}
void SDLCALL SDL_StopTextInput(void) {}
SDL_bool SDLCALL SDL_HasClipboardText(void) { return SDL_FALSE; }
char *SDLCALL SDL_GetClipboardText(void) { char *s = (char *)malloc(1); s[0] = '\0'; return s; }

int SDLCALL SDL_ShowCursor(int toggle) { return 0; }
int SDLCALL SDL_SetRelativeMouseMode(SDL_bool enabled) { return -1; }
void SDLCALL SDL_WarpMouseInWindow(SDL_Window *window, int x, int y) {}
int SDLCALL SDL_GetNumVideoDisplays(void) { return 0; }
const char *SDLCALL SDL_GetDisplayName(int displayIndex) { return ""; }
int SDLCALL SDL_GetWindowDisplayIndex(SDL_Window *window) { return -1; }
Uint32 SDLCALL SDL_GetWindowFlags(SDL_Window *window) { return 0; }
void SDLCALL SDL_GetWindowSize(SDL_Window *window, int *w, int *h) { if(w) *w = 0; if(h) *h = 0; }
void SDLCALL SDL_SetWindowSize(SDL_Window *window, int w, int h) {}
void SDLCALL SDL_SetWindowPosition(SDL_Window *window, int x, int y) {}
int SDLCALL SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags) { return -1; }
void SDLCALL SDL_SetWindowGrab(SDL_Window *window, SDL_bool grabbed) {}
int SDLCALL SDL_SetWindowBrightness(SDL_Window *window, float brightness) { return -1; }
int SDLCALL SDL_GL_SetSwapInterval(int interval) { return -1; }

SDL_Surface *SDLCALL SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
    return newsurface(width, height, depth, 0, NULL, Rmask, Gmask, Bmask, Amask);
}
SDL_Surface *SDLCALL SDL_CreateRGBSurfaceFrom(void *pixels, int width, int height, int depth, int pitch, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
    return newsurface(width, height, depth, pitch, pixels, Rmask, Gmask, Bmask, Amask);
}
void SDLCALL SDL_FreeSurface(SDL_Surface *surface)
{
    if(!surface) return;
    if(!(surface->flags & SDL_PREALLOC)) free(surface->pixels);
    free(surface->format);
    free(surface);
}
int SDLCALL SDL_SetSurfaceBlendMode(SDL_Surface *surface, SDL_BlendMode blendMode) { return 0; }
int SDLCALL SDL_GetColorKey(SDL_Surface *surface, Uint32 *key) { return -1; }

// Whole-surface copy with per-channel format conversion; the engine only blits one
// surface onto a same-sized one to change its pixel format.
int SDLCALL SDL_UpperBlit(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect)
{
    if(!src || !dst) return -1;
    const SDL_PixelFormat *sf = src->format, *df = dst->format;
    int w = min(src->w, dst->w), h = min(src->h, dst->h);
    loop(y, h) loop(x, w)
    {
        Uint32 v = getpixel(src, x, y);
        putpixel(dst, x, y, convertchannel(v, sf->Rmask, df->Rmask, 0) | convertchannel(v, sf->Gmask, df->Gmask, 0) |
                            convertchannel(v, sf->Bmask, df->Bmask, 0) | convertchannel(v, sf->Amask, df->Amask, 0xFF));
    }
    return 0;
}
int SDLCALL SDL_SaveBMP_RW(SDL_Surface *surface, SDL_RWops *dst, int freedst) { if(freedst && dst) SDL_RWclose(dst); return -1; }

SDL_RWops *SDLCALL SDL_AllocRW(void) { return (SDL_RWops *)calloc(1, sizeof(SDL_RWops)); }
void SDLCALL SDL_FreeRW(SDL_RWops *area) { free(area); }
int SDLCALL SDL_RWclose(SDL_RWops *context) { return context && context->close ? context->close(context) : -1; }
SDL_RWops *SDLCALL SDL_RWFromFile(const char *file, const char *mode) { return NULL; }

int SDLCALL SDL_BuildAudioCVT(SDL_AudioCVT *cvt, SDL_AudioFormat src_format, Uint8 src_channels, int src_rate, SDL_AudioFormat dst_format, Uint8 dst_channels, int dst_rate) { return -1; }
int SDLCALL SDL_ConvertAudio(SDL_AudioCVT *cvt) { return -1; }
SDL_AudioSpec *SDLCALL SDL_LoadWAV_RW(SDL_RWops *src, int freesrc, SDL_AudioSpec *spec, Uint8 **audio_buf, Uint32 *audio_len) { if(freesrc && src) SDL_RWclose(src); return NULL; }

int SDLCALL Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunksize) { return -1; }
void SDLCALL Mix_CloseAudio(void) {}
int SDLCALL Mix_QuerySpec(int *frequency, Uint16 *format, int *channels) { return 0; }
int SDLCALL Mix_AllocateChannels(int numchans) { return 0; }
void SDLCALL Mix_SetPostMix(void (SDLCALL *mix_func)(void *udata, Uint8 *stream, int len), void *arg) {}

Mix_Chunk * SDLCALL Mix_LoadWAV_RW(SDL_RWops *src, int freesrc) { if(freesrc && src) SDL_RWclose(src); return NULL; }
Mix_Chunk * SDLCALL Mix_QuickLoad_RAW(Uint8 *mem, Uint32 len) { return NULL; }
void SDLCALL Mix_FreeChunk(Mix_Chunk *chunk) {}
int SDLCALL Mix_PlayChannelTimed(int channel, Mix_Chunk *chunk, int loops, int ticks) { return -1; }
int SDLCALL Mix_FadeInChannelTimed(int channel, Mix_Chunk *chunk, int loops, int ms, int ticks) { return -1; }
int SDLCALL Mix_FadeOutChannel(int which, int ms) { return 0; }
Mix_Fading SDLCALL Mix_FadingChannel(int which) { return MIX_NO_FADING; }
int SDLCALL Mix_HaltChannel(int channel) { return 0; }
int SDLCALL Mix_Playing(int channel) { return 0; }
int SDLCALL Mix_Volume(int channel, int volume) { return 0; }
int SDLCALL Mix_SetPanning(int channel, Uint8 left, Uint8 right) { return 0; }

Mix_Music * SDLCALL Mix_LoadMUS(const char *file) { return NULL; }
Mix_Music * SDLCALL Mix_LoadMUSType_RW(SDL_RWops *src, Mix_MusicType type, int freesrc) { if(freesrc && src) SDL_RWclose(src); return NULL; }
void SDLCALL Mix_FreeMusic(Mix_Music *music) {}
int SDLCALL Mix_PlayMusic(Mix_Music *music, int loops) { return -1; }
int SDLCALL Mix_HaltMusic(void) { return 0; }
void SDLCALL Mix_ResumeMusic(void) {}
int SDLCALL Mix_PausedMusic(void) { return 0; }
int SDLCALL Mix_PlayingMusic(void) { return 0; }
int SDLCALL Mix_VolumeMusic(int volume) { return 0; }

SDL_Surface * SDLCALL IMG_Load(const char *file) { return NULL; }
SDL_Surface * SDLCALL IMG_LoadTyped_RW(SDL_RWops *src, int freesrc, const char *type) { if(freesrc && src) SDL_RWclose(src); return NULL; }
int SDLCALL IMG_SaveJPG_RW(SDL_Surface *surface, SDL_RWops *dst, int freedst, int quality) { if(freedst && dst) SDL_RWclose(dst); return -1; }

}

#endif
