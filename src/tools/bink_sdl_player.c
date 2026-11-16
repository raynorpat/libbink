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
 * Keys: Esc/q quit, Space pauses the picture and the sound,
 * Left/Right seek one second.
 */

#include "bink.h"
#include "tools/bink_mix.h"
#include "tools/bink_playback.h"

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

/* Handle quit, escape, and space. Put every other event back. */
static int poll_wait_input(int *quit, int *paused, BinkPlayback *audio,
                           int *device_paused)
{
    SDL_Event keep[128];
    SDL_Event event;
    int kept = 0;
    int stop = 0;
    int i;

    while (kept < 128 && SDL_PollEvent(&event)) {
        int handled = 0;

        if (!stop && event.type == BINK_EVENT_QUIT) {
            *quit = 1;
            stop = 1;
            handled = 1;
        } else if (!stop && event.type == BINK_EVENT_KEY_DOWN) {
            int key = BINK_EVENT_KEY(event);

            if (key == SDLK_ESCAPE || key == BINK_KEY_Q) {
                *quit = 1;
                stop = 1;
                handled = 1;
            } else if (key == SDLK_SPACE) {
                *paused = !*paused;
                bink_playback_set_paused(audio, *paused, device_paused);
                handled = 1;
                if (*paused)
                    stop = 1;
            }
        }
        if (!handled)
            keep[kept++] = event;
    }
    for (i = 0; i < kept; i++)
        (void)SDL_PushEvent(&keep[i]);
    return stop;
}

/* Hold this picture until `deadline` milliseconds. One picture is
   bink_frame_bytes of audio. The device reports that loss in whole
   buffers, which are larger than the leftover of a frame, so waiting
   until the counter crosses the threshold plays longer than a picture
   and the cushion drains. An empty queue returns at once: sleeping
   would only widen the hole. */
static void wait_for_audio(BinkPlayback *audio, double usf, Uint64 deadline,
                           int *quit, int *paused, int *device_paused)
{
    unsigned long frame_bytes = bink_frame_bytes(
        (unsigned long)audio->rate, audio->channels, usf);
    unsigned long mark = bink_playback_queued(audio);

    for (;;) {
        unsigned long queued;
        Uint64 now;
        Uint64 left;

        now = SDL_GetTicks();
        if (now >= deadline)
            return;
        if (poll_wait_input(quit, paused, audio, device_paused))
            return;
        queued = bink_playback_queued(audio);
        if (!bink_audio_waiting(mark, queued, frame_bytes) && queued == 0)
            return;
        now = SDL_GetTicks();
        if (now >= deadline)
            return;
        left = deadline - now;
        if (left > 5)
            left = 5;
        SDL_Delay((Uint32)left);
    }
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
    BinkPlayback audio;
    unsigned long audio_bytes = 0;
    int audio_inited = 0;
    int need_audio = 1;
    int quit = 0;
    int paused = 0;
    int device_paused = 0;
    double picture_ms = 0.0;
    double next_due = 0.0;
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
        if (bink_playback_audio_subsystem() != 0) {
            fprintf(stderr, "%s\n", SDL_GetError());
        } else {
            audio_inited = 1;
            if (bink_playback_audio_open(&audio, audio.rate, audio.channels) != 0)
                fprintf(stderr, "%s\n", SDL_GetError());
            else
                device_paused = 1;
        }
    }

    status = bink_first(b);
    if (status == BINK_ERROR || status == BINK_DONE) {
        fprintf(stderr, "Failed to decode the first frame of %s.\n", path);
        bink_playback_audio_close(&audio);
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
                bink_playback_seek(b, target, &status, &audio, usf, enabled,
                              channels, rates) != 0) {
                bink_playback_audio_close(&audio);
                if (audio_inited)
                    SDL_QuitSubSystem(SDL_INIT_AUDIO);
                bink_close(b);
                return 1;
            }
            bink_info_all(b, &cur, NULL, NULL);
            printf("seeked frame %lu\n", cur);
            bink_playback_audio_close(&audio);
            if (audio_inited)
                SDL_QuitSubSystem(SDL_INIT_AUDIO);
            bink_close(b);
            return cur == target ? 0 : 1;
        }
    }

    if (video_init() != 0) {
        if (test_frames == 0) {
            fprintf(stderr, "SDL video init failed: %s\n", SDL_GetError());
            bink_playback_audio_close(&audio);
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
            bink_playback_publish(b, enabled, channels, rates, &audio, &audio_bytes);
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
                    if (bink_playback_seek(b, (unsigned long)target, &status,
                                      &audio, usf, enabled, channels,
                                      rates) == 0) {
                        redraw = 1;
                        need_audio = 0;
                        next_due = 0.0;
                    }
                }
            }
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
            bink_playback_set_paused(&audio, 1, &device_paused);
            SDL_Delay(10);
            continue;
        }
        if (device_paused)
            bink_playback_set_paused(&audio, 0, &device_paused);

        if (test_frames != 0 && decoded >= test_frames)
            break;

        if (status == BINK_LAST)
            break;

        if (audio.ready && test_frames == 0) {
            double now = (double)SDL_GetTicks();

            if (picture_ms == 0.0) {
                unsigned long frame_bytes = bink_frame_bytes(
                    (unsigned long)audio.rate, audio.channels, usf);
                double byte_rate = (double)audio.rate *
                                   (double)audio.channels * 2.0;

                picture_ms = frame_ms;
                if (frame_bytes > 0 && byte_rate > 0.0)
                    picture_ms = (double)frame_bytes / byte_rate * 1000.0;
            }
            if (next_due == 0.0)
                next_due = now;
            else if (now - next_due > picture_ms)
                next_due = now;
            next_due += picture_ms;
            if (next_due > now)
                wait_for_audio(&audio, usf, (Uint64)(next_due + 0.5),
                               &quit, &paused, &device_paused);
        } else if (!headless)
            wait_until((double)SDL_GetTicks() + frame_ms);

        if (quit)
            break;
        if (paused)
            continue;

        status = bink_next(b);
        if (status == BINK_ERROR || status == BINK_DONE)
            break;
        decoded++;
        need_audio = 1;
    }

done:
    if (test_frames != 0)
        printf("audio bytes %lu\n", audio_bytes);
    bink_playback_audio_close(&audio);
    bink_playback_shutdown();
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
