/*
 * libbink - SDL2/SDL3 playback example
 *
 * Build with the tools enabled:
 *   cmake -S . -B build -DBINK_BUILD_TOOLS=ON
 *   cmake --build build
 *
 * CMake selects SDL3 when available and falls back to SDL2. Force one with
 * -DBINK_SDL_VERSION=3 or -DBINK_SDL_VERSION=2.
 *
 * Usage:
 *   bink_sdl_player file.bik
 *
 * Keys: Esc/q quit, Space pause, Left/Right seek one second.
 */

#include "bink.h"
#include "tools/bink_mix.h"

#ifdef BINK_USE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef BINK_USE_SDL3
#define BINK_EVENT_QUIT SDL_EVENT_QUIT
#define BINK_EVENT_KEY_DOWN SDL_EVENT_KEY_DOWN
#define BINK_EVENT_KEY(event) ((event).key.key)
#define BINK_KEY_Q SDLK_Q
#else
#define BINK_EVENT_QUIT SDL_QUIT
#define BINK_EVENT_KEY_DOWN SDL_KEYDOWN
#define BINK_EVENT_KEY(event) ((event).key.keysym.sym)
#define BINK_KEY_Q SDLK_q
#endif

static int video_init(void)
{
#ifdef BINK_USE_SDL3
    return SDL_Init(SDL_INIT_VIDEO) ? 0 : -1;
#else
    return SDL_Init(SDL_INIT_VIDEO) == 0 ? 0 : -1;
#endif
}

static void present(SDL_Renderer *renderer, SDL_Texture *texture)
{
    SDL_RenderClear(renderer);
#ifdef BINK_USE_SDL3
    SDL_RenderTexture(renderer, texture, NULL, NULL);
#else
    SDL_RenderCopy(renderer, texture, NULL, NULL);
#endif
    SDL_RenderPresent(renderer);
}

static void wait_until(double target_ms)
{
    double now = (double)SDL_GetTicks();

    if (target_ms > now)
        SDL_Delay((Uint32)(target_ms - now + 0.999));
}

typedef struct {
    int ready;
    int rate;
    int channels;
#ifdef BINK_USE_SDL3
    SDL_AudioStream *stream;
#else
    SDL_AudioDeviceID device;
#endif
} PlayerAudio;

static int16_t *mix_pcm = NULL;
static unsigned long mix_cap = 0;

static int audio_subsystem(void)
{
#ifdef BINK_USE_SDL3
    return SDL_InitSubSystem(SDL_INIT_AUDIO) ? 0 : -1;
#else
    return SDL_InitSubSystem(SDL_INIT_AUDIO) == 0 ? 0 : -1;
#endif
}

static int audio_open(PlayerAudio *audio, int rate, int channels)
{
    SDL_AudioSpec spec;

    memset(&spec, 0, sizeof(spec));
    audio->rate = rate;
    audio->channels = channels;
    audio->ready = 0;
#ifdef BINK_USE_SDL3
    spec.format = SDL_AUDIO_S16;
    spec.channels = channels;
    spec.freq = rate;
    audio->stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio->stream == NULL)
        return -1;
    if (!SDL_ResumeAudioStreamDevice(audio->stream)) {
        SDL_DestroyAudioStream(audio->stream);
        audio->stream = NULL;
        return -1;
    }
#else
    spec.freq = rate;
    spec.format = AUDIO_S16SYS;
    spec.channels = (Uint8)channels;
    spec.samples = 4096;
    spec.callback = NULL;
    audio->device = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
    if (audio->device == 0)
        return -1;
    SDL_PauseAudioDevice(audio->device, 0);
#endif
    audio->ready = 1;
    return 0;
}

static void audio_close(PlayerAudio *audio)
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

static void audio_clear(PlayerAudio *audio)
{
    if (!audio->ready)
        return;
#ifdef BINK_USE_SDL3
    SDL_ClearAudioStream(audio->stream);
#else
    SDL_ClearQueuedAudio(audio->device);
#endif
}

static unsigned long audio_queued(const PlayerAudio *audio)
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

static void audio_queue(PlayerAudio *audio, const void *data, unsigned long bytes)
{
    if (!audio->ready || data == NULL || bytes == 0)
        return;
#ifdef BINK_USE_SDL3
    SDL_PutAudioStreamData(audio->stream, data, (int)bytes);
#else
    SDL_QueueAudio(audio->device, data, (Uint32)bytes);
#endif
}

static void publish_audio(bink object, const int enabled[7],
                          const unsigned char track_channels[7],
                          const unsigned long track_rates[7],
                          PlayerAudio *audio, unsigned long *total_bytes)
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
        return;

    samples_out = frames_out * (unsigned long)audio->channels;
    if (samples_out > mix_cap) {
        int16_t *grown = (int16_t *)realloc(
            mix_pcm, (size_t)samples_out * sizeof(int16_t));

        if (grown == NULL)
            return;
        mix_pcm = grown;
        mix_cap = samples_out;
    }
    if (bink_mix(tracks, count, (unsigned long)audio->rate, audio->channels,
                 mix_pcm, frames_out) != 0)
        return;
    audio_queue(audio, mix_pcm, samples_out * sizeof(int16_t));
}

static void wait_for_audio(const PlayerAudio *audio, double usf)
{
    double limit = (double)audio->rate * (double)audio->channels * 2.0 *
                   usf / 1000000.0 * 2.0;

    while ((double)audio_queued(audio) > limit)
        SDL_Delay(5);
}

static int seek_to_frame(bink object, unsigned long target, char *status,
                         PlayerAudio *audio)
{
    unsigned long cur = 0;
    unsigned long frames = 0;

    audio_clear(audio);
    if (bink_seek_keyframe(object, target) != 0)
        return -1;
    if (bink_info_all(object, &cur, &frames, NULL) != 0)
        return -1;
    while (cur < target) {
        char next = bink_next(object);

        if (next != BINK_MORE && next != BINK_LAST)
            return -1;
        if (bink_info_all(object, &cur, &frames, NULL) != 0)
            return -1;
        *status = next;
    }
    if (frames > 0 && cur + 1 >= frames)
        *status = BINK_LAST;
    else
        *status = BINK_MORE;
    return 0;
}

static int update_texture(SDL_Texture *texture, bink b)
{
    const unsigned char *y = bink_get_plane(b, BINK_PLANE_Y);
    const unsigned char *u = bink_get_plane(b, BINK_PLANE_U);
    const unsigned char *v = bink_get_plane(b, BINK_PLANE_V);

    if (y == NULL || u == NULL || v == NULL)
        return -1;
#ifdef BINK_USE_SDL3
    if (!SDL_UpdateYUVTexture(texture, NULL,
                              y, bink_get_stride(b, BINK_PLANE_Y),
                              u, bink_get_stride(b, BINK_PLANE_U),
                              v, bink_get_stride(b, BINK_PLANE_V)))
        return -1;
#else
    if (SDL_UpdateYUVTexture(texture, NULL,
                             y, bink_get_stride(b, BINK_PLANE_Y),
                             u, bink_get_stride(b, BINK_PLANE_U),
                             v, bink_get_stride(b, BINK_PLANE_V)) != 0)
        return -1;
#endif
    return 0;
}

int main(int argc, char **argv)
{
    const char *path;
    bink b;
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    unsigned long w = 0;
    unsigned long h = 0;
    unsigned long frames = 0;
    double usf = 0.0;
    unsigned char track_mask = 0;
    unsigned char channels[7];
    unsigned char bitdepth[7];
    unsigned long rates[7];
    unsigned long test_frames = 0;
    unsigned long decoded = 1;
    char title[512];
    char status;
    int enabled[7];
    PlayerAudio audio;
    unsigned long audio_bytes = 0;
    int audio_inited = 0;
    int need_audio = 1;
    int landed = 0;
    int quit = 0;
    int paused = 0;
    int redraw = 1;
    int headless = 0;
    int exit_code = 0;
    int i;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s file.bik\n", argv[0]);
        return 1;
    }
    path = argv[1];

    {
        const char *env = getenv("BINK_SDL_TEST_FRAMES");

        if (env != NULL && env[0] != '\0') {
            char *end = NULL;
            unsigned long value = strtoul(env, &end, 10);

            if (end != env && value > 0)
                test_frames = value;
        }
    }

    b = bink_open_file(path, BINK_MODE_DISK);
    if (b == NULL) {
        fprintf(stderr, "Errors encountered opening %s, exiting.\n", path);
        return 1;
    }

    bink_info_all(b, NULL, &frames, &usf);
    bink_info_video(b, &w, &h, NULL);
    if (w == 0 || h == 0) {
        fprintf(stderr, "%s has no video track, exiting.\n", path);
        bink_close(b);
        return 1;
    }
    if (usf <= 0.0)
        usf = 1000000.0 / 30.0;

    printf("Opened file %s\nWidth: %lu\nHeight: %lu\nFrames: %lu\nFPS: %f\n",
           path, w, h, frames, 1000000.0 / usf);

    memset(enabled, 0, sizeof(enabled));
    memset(&audio, 0, sizeof(audio));
    bink_info_audio(b, &track_mask, channels, bitdepth, rates);

    if (bink_enable_video(b, 1) != 0) {
        fprintf(stderr, "Failed to enable video decoding.\n");
        bink_close(b);
        return 1;
    }

    for (i = 0; i < 7; i++) {
        if ((track_mask & (1u << i)) == 0)
            continue;
        if (bink_enable_audio(b, (unsigned char)i, 1) != 0) {
            fprintf(stderr, "Failed to enable audio track %d.\n", i);
            continue;
        }
        enabled[i] = 1;
        if (audio.rate == 0)
            audio.rate = (int)rates[i];
        if (channels[i] > 1)
            audio.channels = 2;
    }
    if (audio.channels == 0)
        audio.channels = 1;
    if (audio.rate > 0) {
        if (audio_subsystem() != 0) {
            fprintf(stderr, "%s\n", SDL_GetError());
        } else {
            audio_inited = 1;
            if (audio_open(&audio, audio.rate, audio.channels) != 0)
                fprintf(stderr, "%s\n", SDL_GetError());
        }
    }

    status = bink_first(b);
    if (status == BINK_ERROR || status == BINK_DONE) {
        fprintf(stderr, "Failed to decode the first frame of %s.\n", path);
        audio_close(&audio);
        if (audio_inited)
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        bink_close(b);
        return 1;
    }

    {
        const char *seek_env = getenv("BINK_SDL_TEST_SEEK");

        if (seek_env != NULL && seek_env[0] != '\0') {
            char *end = NULL;
            unsigned long target = strtoul(seek_env, &end, 10);
            unsigned long cur = 0;

            if (end == seek_env ||
                seek_to_frame(b, target, &status, &audio) != 0) {
                audio_close(&audio);
                if (audio_inited)
                    SDL_QuitSubSystem(SDL_INIT_AUDIO);
                bink_close(b);
                return 1;
            }
            bink_info_all(b, &cur, NULL, NULL);
            printf("seeked frame %lu\n", cur);
            audio_close(&audio);
            if (audio_inited)
                SDL_QuitSubSystem(SDL_INIT_AUDIO);
            bink_close(b);
            return cur == target ? 0 : 1;
        }
    }

    if (video_init() != 0) {
        if (test_frames == 0) {
            fprintf(stderr, "SDL video init failed: %s\n", SDL_GetError());
            audio_close(&audio);
            if (audio_inited)
                SDL_QuitSubSystem(SDL_INIT_AUDIO);
            bink_close(b);
            return 1;
        }
        headless = 1;
    }

    if (!headless) {
        snprintf(title, sizeof(title), "libbink SDL player - %s", path);
#ifdef BINK_USE_SDL3
        window = SDL_CreateWindow(title, (int)w, (int)h,
                                  SDL_WINDOW_RESIZABLE);
        if (window != NULL)
            renderer = SDL_CreateRenderer(window, NULL);
#else
        window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED, (int)w, (int)h,
                                  SDL_WINDOW_RESIZABLE);
        if (window != NULL) {
            renderer = SDL_CreateRenderer(window, -1,
                                          SDL_RENDERER_ACCELERATED);
            if (renderer == NULL)
                renderer = SDL_CreateRenderer(window, -1, 0);
        }
#endif
        if (window == NULL || renderer == NULL) {
            if (test_frames > 0) {
                headless = 1;
            } else {
                fprintf(stderr, "SDL window/renderer creation failed: %s\n",
                        SDL_GetError());
                exit_code = 1;
                goto done;
            }
        } else {
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        (int)w, (int)h);
            if (texture == NULL) {
                if (test_frames > 0) {
                    headless = 1;
                } else {
                    fprintf(stderr, "SDL texture creation failed: %s\n",
                            SDL_GetError());
                    exit_code = 1;
                    goto done;
                }
            } else {
#ifdef BINK_USE_SDL3
                SDL_SetRenderLogicalPresentation(renderer, (int)w, (int)h,
                                                 SDL_LOGICAL_PRESENTATION_LETTERBOX);
#else
                SDL_RenderSetLogicalSize(renderer, (int)w, (int)h);
#endif
            }
        }
    }

    while (!quit) {
        double frame_ms = usf / 1000.0;

        if (need_audio) {
            publish_audio(b, enabled, channels, rates, &audio, &audio_bytes);
            need_audio = 0;
        }

        if (!headless) {
            SDL_Event event;

            while (SDL_PollEvent(&event)) {
                int key;

                if (event.type == BINK_EVENT_QUIT) {
                    quit = 1;
                    break;
                }
                if (event.type != BINK_EVENT_KEY_DOWN)
                    continue;

                key = BINK_EVENT_KEY(event);
                if (key == SDLK_ESCAPE || key == BINK_KEY_Q) {
                    quit = 1;
                } else if (key == SDLK_SPACE) {
                    paused = !paused;
                    redraw = 1;
                } else if (key == SDLK_LEFT || key == SDLK_RIGHT) {
                    unsigned long cur = 0;
                    unsigned long step;
                    long target;

                    bink_info_all(b, &cur, &frames, NULL);
                    step = (unsigned long)(1000000.0 / usf + 0.5);
                    if (step < 1)
                        step = 1;
                    target = (long)cur +
                             (key == SDLK_LEFT ? -(long)step : (long)step);
                    if (target < 0)
                        target = 0;
                    if (frames > 0 && target >= (long)frames)
                        target = (long)frames - 1;
                    if (seek_to_frame(b, (unsigned long)target, &status,
                                      &audio) == 0) {
                        redraw = 1;
                        landed = 1;
                    }
                }
            }
        }
        if (landed) {
            publish_audio(b, enabled, channels, rates, &audio, &audio_bytes);
            landed = 0;
        }
        if (quit)
            break;

        if (!headless && (!paused || redraw)) {
            if (update_texture(texture, b) != 0) {
                fprintf(stderr, "Failed to update the video texture: %s\n",
                        SDL_GetError());
                exit_code = 1;
                break;
            }
            present(renderer, texture);
            redraw = 0;
        }

        if (paused) {
            SDL_Delay(10);
            continue;
        }

        if (test_frames != 0 && decoded >= test_frames)
            break;

        if (status == BINK_LAST)
            break;

        if (audio.ready && test_frames == 0)
            wait_for_audio(&audio, usf);
        else if (!headless)
            wait_until((double)SDL_GetTicks() + frame_ms);

        status = bink_next(b);
        if (status == BINK_ERROR || status == BINK_DONE)
            break;
        decoded++;
        need_audio = 1;
    }

done:
    if (test_frames != 0)
        printf("audio bytes %lu\n", audio_bytes);
    audio_close(&audio);
    free(mix_pcm);
    mix_pcm = NULL;
    mix_cap = 0;
    if (texture != NULL)
        SDL_DestroyTexture(texture);
    if (renderer != NULL)
        SDL_DestroyRenderer(renderer);
    if (window != NULL)
        SDL_DestroyWindow(window);
    SDL_Quit();
    bink_close(b);

    if (status == BINK_ERROR)
        exit_code = 1;
    return exit_code;
}
