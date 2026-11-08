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

static int seek_to_frame(bink object, unsigned long target, char *status)
{
    unsigned long cur = 0;
    unsigned long frames = 0;

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
    int quit = 0;
    int paused = 0;
    int redraw = 1;
    int headless = 0;
    int exit_code = 0;

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

    bink_info_audio(b, &track_mask, channels, bitdepth, rates);
    if (track_mask != 0)
        printf("Audio tracks present; audio decoding is not implemented in "
               "this version.\n");

    if (bink_enable_video(b, 1) != 0) {
        fprintf(stderr, "Failed to enable video decoding.\n");
        bink_close(b);
        return 1;
    }

    status = bink_first(b);
    if (status == BINK_ERROR || status == BINK_DONE) {
        fprintf(stderr, "Failed to decode the first frame of %s.\n", path);
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
                seek_to_frame(b, target, &status) != 0) {
                bink_close(b);
                return 1;
            }
            bink_info_all(b, &cur, NULL, NULL);
            printf("seeked frame %lu\n", cur);
            bink_close(b);
            return cur == target ? 0 : 1;
        }
    }

    if (video_init() != 0) {
        if (test_frames == 0) {
            fprintf(stderr, "SDL video init failed: %s\n", SDL_GetError());
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
                    if (seek_to_frame(b, (unsigned long)target, &status) == 0)
                        redraw = 1;
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
            SDL_Delay(10);
            continue;
        }

        if (test_frames != 0 && decoded >= test_frames)
            break;

        if (status == BINK_LAST)
            break;

        if (!headless)
            wait_until((double)SDL_GetTicks() + frame_ms);

        status = bink_next(b);
        if (status == BINK_ERROR || status == BINK_DONE)
            break;
        decoded++;
    }

done:
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
