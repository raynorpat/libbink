/* SDL backend for the shared playback module. */

#include "tools/bink_playback.h"

#ifdef BINK_USE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

#include <stdlib.h>
#include <string.h>

typedef struct {
#ifdef BINK_USE_SDL3
    SDL_AudioStream *stream;
#else
    SDL_AudioDeviceID device;
#endif
} SdlDevice;

static SdlDevice *sdl_of(BinkPlayback *p)
{
    return (SdlDevice *)p->device;
}

int bink_playback_audio_subsystem(void)
{
#ifdef BINK_USE_SDL3
    return SDL_InitSubSystem(SDL_INIT_AUDIO) ? 0 : -1;
#else
    return SDL_InitSubSystem(SDL_INIT_AUDIO) == 0 ? 0 : -1;
#endif
}

int bink_playback_audio_open(BinkPlayback *p, int rate, int channels)
{
    SDL_AudioSpec spec;
    SdlDevice *sdl;

    memset(&spec, 0, sizeof(spec));
    p->rate = rate;
    p->channels = channels;
    p->ready = 0;
    sdl = (SdlDevice *)calloc(1, sizeof(*sdl));
    if (sdl == NULL)
        return -1;
#ifdef BINK_USE_SDL3
    /* Host order. Spell both formats so a later edit does not pin the
       device to little-endian while the library publishes native s16. */
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
    spec.format = SDL_AUDIO_S16LE;
#else
    spec.format = SDL_AUDIO_S16BE;
#endif
    spec.channels = channels;
    spec.freq = rate;
    sdl->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                            &spec, NULL, NULL);
    if (sdl->stream == NULL) {
        free(sdl);
        return -1;
    }
#else
    spec.freq = rate;
    /* AUDIO_S16SYS is S16LSB on a little-endian host and S16MSB on a
       big-endian host, matching native s16 from bink_get_audio. */
    spec.format = AUDIO_S16SYS;
    spec.channels = (Uint8)channels;
    spec.samples = 4096;
    spec.callback = NULL;
    sdl->device = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
    if (sdl->device == 0) {
        free(sdl);
        return -1;
    }
#endif
    /* Stay paused until the first packet is queued. An empty running
       device crackles until that packet arrives. */
    p->device = sdl;
    p->ready = 1;
    return 0;
}

void bink_playback_audio_close(BinkPlayback *p)
{
    SdlDevice *sdl = sdl_of(p);

    if (!p->ready || sdl == NULL)
        return;
#ifdef BINK_USE_SDL3
    SDL_DestroyAudioStream(sdl->stream);
#else
    SDL_CloseAudioDevice(sdl->device);
#endif
    free(sdl);
    p->device = NULL;
    p->ready = 0;
}

void bink_playback_audio_clear(BinkPlayback *p)
{
    SdlDevice *sdl = sdl_of(p);

    if (!p->ready || sdl == NULL)
        return;
#ifdef BINK_USE_SDL3
    SDL_ClearAudioStream(sdl->stream);
#else
    SDL_ClearQueuedAudio(sdl->device);
#endif
}

unsigned long bink_playback_queued(const BinkPlayback *p)
{
    SdlDevice *sdl = (SdlDevice *)p->device;

    if (!p->ready || sdl == NULL)
        return 0;
#ifdef BINK_USE_SDL3
    {
        int queued = SDL_GetAudioStreamQueued(sdl->stream);

        return queued > 0 ? (unsigned long)queued : 0;
    }
#else
    return (unsigned long)SDL_GetQueuedAudioSize(sdl->device);
#endif
}

void bink_playback_queue(BinkPlayback *p, const void *data, unsigned long bytes)
{
    SdlDevice *sdl = sdl_of(p);

    if (!p->ready || sdl == NULL || data == NULL || bytes == 0)
        return;
#ifdef BINK_USE_SDL3
    SDL_PutAudioStreamData(sdl->stream, data, (int)bytes);
#else
    SDL_QueueAudio(sdl->device, data, (Uint32)bytes);
#endif
}

void bink_playback_set_paused(BinkPlayback *p, int pause_on, int *device_paused)
{
    SdlDevice *sdl = sdl_of(p);

    if (!p->ready || sdl == NULL || pause_on == *device_paused)
        return;
#ifdef BINK_USE_SDL3
    if (pause_on)
        (void)SDL_PauseAudioStreamDevice(sdl->stream);
    else
        (void)SDL_ResumeAudioStreamDevice(sdl->stream);
#else
    SDL_PauseAudioDevice(sdl->device, pause_on ? 1 : 0);
#endif
    *device_paused = pause_on;
}
