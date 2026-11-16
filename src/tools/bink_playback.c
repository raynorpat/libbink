#include "tools/bink_playback.h"

#include "tools/bink_mix.h"

#include <stdlib.h>
#include <string.h>

static int16_t *mix_pcm = NULL;
static unsigned long mix_cap = 0;

int bink_playback_audio_subsystem(void)
{
#ifdef BINK_USE_SDL3
    return SDL_InitSubSystem(SDL_INIT_AUDIO) ? 0 : -1;
#else
    return SDL_InitSubSystem(SDL_INIT_AUDIO) == 0 ? 0 : -1;
#endif
}

int bink_playback_audio_open(BinkPlayback *audio, int rate, int channels)
{
    SDL_AudioSpec spec;

    memset(&spec, 0, sizeof(spec));
    audio->rate = rate;
    audio->channels = channels;
    audio->ready = 0;
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
    audio->stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio->stream == NULL)
        return -1;
#else
    spec.freq = rate;
    /* AUDIO_S16SYS is S16LSB on a little-endian host and S16MSB on a
       big-endian host, matching native s16 from bink_get_audio. */
    spec.format = AUDIO_S16SYS;
    spec.channels = (Uint8)channels;
    spec.samples = 4096;
    spec.callback = NULL;
    audio->device = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
    if (audio->device == 0)
        return -1;
#endif
    /* Stay paused until the first packet is queued. An empty running
       device crackles until that packet arrives. */
    audio->ready = 1;
    return 0;
}

void bink_playback_audio_close(BinkPlayback *audio)
{
    if (!audio->ready)
        return;
#ifdef BINK_USE_SDL3
    SDL_DestroyAudioStream(audio->stream);
    audio->stream = NULL;
#else
    SDL_CloseAudioDevice(audio->device);
    audio->device = 0;
#endif
    audio->ready = 0;
}

void bink_playback_audio_clear(BinkPlayback *audio)
{
    if (!audio->ready)
        return;
#ifdef BINK_USE_SDL3
    SDL_ClearAudioStream(audio->stream);
#else
    SDL_ClearQueuedAudio(audio->device);
#endif
}

unsigned long bink_playback_queued(const BinkPlayback *audio)
{
    if (!audio->ready)
        return 0;
#ifdef BINK_USE_SDL3
    {
        int queued = SDL_GetAudioStreamQueued(audio->stream);

        return queued > 0 ? (unsigned long)queued : 0;
    }
#else
    return (unsigned long)SDL_GetQueuedAudioSize(audio->device);
#endif
}

void bink_playback_queue(BinkPlayback *audio, const void *data,
                         unsigned long bytes)
{
    if (!audio->ready || data == NULL || bytes == 0)
        return;
#ifdef BINK_USE_SDL3
    SDL_PutAudioStreamData(audio->stream, data, (int)bytes);
#else
    SDL_QueueAudio(audio->device, data, (Uint32)bytes);
#endif
}

void bink_playback_set_paused(BinkPlayback *audio, int pause_on,
                              int *device_paused)
{
    if (!audio->ready || pause_on == *device_paused)
        return;
#ifdef BINK_USE_SDL3
    if (pause_on)
        (void)SDL_PauseAudioStreamDevice(audio->stream);
    else
        (void)SDL_ResumeAudioStreamDevice(audio->stream);
#else
    SDL_PauseAudioDevice(audio->device, pause_on ? 1 : 0);
#endif
    *device_paused = pause_on;
}

/* Mix the current frame into mix_pcm. Returns the byte count. A NULL
   total_bytes skips the test counter. */
static unsigned long mix_current(bink object, const int enabled[7],
                                 const unsigned char track_channels[7],
                                 const unsigned long track_rates[7],
                                 const BinkPlayback *audio,
                                 unsigned long *total_bytes)
{
    BinkMixTrack tracks[7];
    unsigned long frames_out = 0;
    unsigned long samples_out;
    int count = 0;
    int i;

    for (i = 0; i < 7; i++) {
        unsigned long size;
        unsigned long samples = 0;
        const unsigned char *pcm = NULL;

        if (!enabled[i])
            continue;
        size = bink_get_audio_size(object, (unsigned char)i);
        if (total_bytes != NULL)
            *total_bytes += size;
        if (size > 0 && track_channels[i] > 0) {
            samples = size / ((unsigned long)track_channels[i] * sizeof(int16_t));
            pcm = bink_get_audio(object, (unsigned char)i);
            if (pcm == NULL)
                samples = 0;
        }
        tracks[count].pcm = (const int16_t *)pcm;
        tracks[count].samples = samples;
        tracks[count].rate = track_rates[i];
        tracks[count].channels = track_channels[i];
        if (samples > 0 && track_rates[i] > 0 && audio->rate > 0) {
            unsigned long converted =
                (unsigned long)(((unsigned long long)samples *
                                 (unsigned long long)audio->rate) /
                                track_rates[i]);

            if (converted > frames_out)
                frames_out = converted;
        }
        count++;
    }
    if (count == 0 || frames_out == 0 || audio->channels <= 0)
        return 0;

    samples_out = frames_out * (unsigned long)audio->channels;
    if (samples_out > mix_cap) {
        int16_t *grown = (int16_t *)realloc(
            mix_pcm, (size_t)samples_out * sizeof(int16_t));

        if (grown == NULL)
            return 0;
        mix_pcm = grown;
        mix_cap = samples_out;
    }
    if (bink_mix(tracks, count, (unsigned long)audio->rate, audio->channels,
                 mix_pcm, frames_out) != 0)
        return 0;
    return samples_out * sizeof(int16_t);
}

void bink_playback_publish(bink object, const int enabled[7],
                           const unsigned char track_channels[7],
                           const unsigned long track_rates[7],
                           BinkPlayback *audio, unsigned long *total_bytes)
{
    unsigned long bytes = mix_current(object, enabled, track_channels,
                                      track_rates, audio, total_bytes);

    if (bytes > 0)
        bink_playback_queue(audio, mix_pcm, bytes);
}

static int append_pcm(unsigned char **stream, unsigned long *length,
                      unsigned long *cap, unsigned long bytes)
{
    unsigned char *grown;
    unsigned long need;

    if (bytes == 0)
        return 0;
    if (*length > (unsigned long)-1 - bytes)
        return -1;
    need = *length + bytes;
    if (need > *cap) {
        unsigned long next = *cap == 0 ? need : *cap;

        while (next < need) {
            if (next > (unsigned long)-1 / 2)
                return -1;
            next *= 2;
        }
        grown = (unsigned char *)realloc(*stream, (size_t)next);
        if (grown == NULL)
            return -1;
        *stream = grown;
        *cap = next;
    }
    memcpy(*stream + *length, mix_pcm, (size_t)bytes);
    *length += bytes;
    return 0;
}

int bink_playback_seek(bink object, unsigned long target, char *status,
                       BinkPlayback *audio, double usf, const int enabled[7],
                       const unsigned char track_channels[7],
                       const unsigned long track_rates[7])
{
    unsigned long cur = 0;
    unsigned long frames = 0;
    unsigned long origin = 0;
    unsigned char *stream = NULL;
    unsigned long length = 0;
    unsigned long cap = 0;

    bink_playback_audio_clear(audio);
    if (bink_seek_keyframe(object, target) != 0)
        return -1;
    if (bink_info_all(object, &cur, &frames, NULL) != 0)
        return -1;
    origin = cur;

    if (audio->ready) {
        unsigned long bytes = mix_current(object, enabled, track_channels,
                                          track_rates, audio, NULL);

        if (append_pcm(&stream, &length, &cap, bytes) != 0) {
            free(stream);
            return -1;
        }
    }
    while (cur < target) {
        char next = bink_next(object);

        if (next != BINK_MORE && next != BINK_LAST) {
            free(stream);
            return -1;
        }
        if (bink_info_all(object, &cur, &frames, NULL) != 0) {
            free(stream);
            return -1;
        }
        *status = next;
        if (!audio->ready)
            continue;
        {
            unsigned long bytes = mix_current(object, enabled, track_channels,
                                              track_rates, audio, NULL);

            if (append_pcm(&stream, &length, &cap, bytes) != 0) {
                free(stream);
                return -1;
            }
        }
    }

    if (audio->ready && length > 0) {
        unsigned long offset = bink_soundtrack_offset(
            (unsigned long)audio->rate, audio->channels, usf, target);
        unsigned long start = bink_soundtrack_offset(
            (unsigned long)audio->rate, audio->channels, usf, origin);

        if (offset >= start)
            offset -= start;
        else
            offset = 0;
        if (offset < length)
            bink_playback_queue(audio, stream + offset, length - offset);
    }
    free(stream);

    if (frames > 0 && cur + 1 >= frames)
        *status = BINK_LAST;
    else
        *status = BINK_MORE;
    return 0;
}

void bink_playback_shutdown(void)
{
    free(mix_pcm);
    mix_pcm = NULL;
    mix_cap = 0;
}
