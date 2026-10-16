// SDL_mixer and SDL_image functions that do nothing, for the headless re-simulation build
// (sauer_resim), which links neither library. Sound never starts (Mix_OpenAudio fails) and
// images are never decoded, so these only need to fail or report "nothing playing".

#ifdef HEADLESS

#include "engine.h"
#ifdef __APPLE__
  #include <SDL2_mixer/SDL_mixer.h>
  #include <SDL2_image/SDL_image.h>
#else
  #include "SDL_mixer.h"
  #include "SDL_image.h"
#endif

extern "C" {

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
