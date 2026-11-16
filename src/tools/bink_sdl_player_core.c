/*
 * Shared SDL player loop: window, audio, frame pair, keys and hooks.
 * A renderer backend supplies the GPU half.
 */

#include "tools/bink_sdl_player_core.h"

#include "bink.h"
#include "tools/bink_mix.h"
#include "tools/bink_playback.h"

#ifdef BINK_USE_SDL3
#include <SDL3/SDL.h>
#define BINK_EVENT_QUIT SDL_EVENT_QUIT
#define BINK_EVENT_KEY_DOWN SDL_EVENT_KEY_DOWN
#define BINK_EVENT_KEY(event) ((event).key.key)
#define BINK_KEY_Q SDLK_Q
#define BINK_KEY_M SDLK_M
#else
#include <SDL.h>
#define BINK_EVENT_QUIT SDL_QUIT
#define BINK_EVENT_KEY_DOWN SDL_KEYDOWN
#define BINK_EVENT_KEY(event) ((event).key.keysym.sym)
#define BINK_KEY_Q SDLK_q
#define BINK_KEY_M SDLK_m
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int dump_enabled;
static unsigned char *slot_rgb[2];

static void wait_until(double target_ms)
{
    double now = (double)SDL_GetTicks();

    if (target_ms > now)
        SDL_Delay((Uint32)(target_ms - now + 0.999));
}

static void upload_slot(BinkRenderer *renderer,
                        const BinkRendererBackend *backend, bink object,
                        int slot, unsigned long w, unsigned long h)
{
    const unsigned char *packed;

    backend->upload(renderer, object, slot);
    if (!dump_enabled || slot_rgb[slot] == NULL)
        return;
    packed = bink_get_video(object);
    if (packed != NULL)
        memcpy(slot_rgb[slot], packed, (size_t)w * (size_t)h * 3u);
}

/* Write the readback image and report how far it is from the expected
 * frame, pixel by pixel. */
static void check_readback(BinkRenderer *renderer,
                           const BinkRendererBackend *backend,
                           const unsigned char *expect, const char *path,
                           unsigned long w, unsigned long h)
{
    unsigned char *pixels;
    unsigned long i;
    double sum = 0.0;
    double count = 0.0;
    int worst = 0;

    if (!dump_enabled || expect == NULL)
        return;
    pixels = (unsigned char *)malloc((size_t)w * (size_t)h * 3u);
    if (pixels == NULL)
        return;
    if (backend->readback(renderer, pixels, w, h) != 0) {
        fprintf(stderr, "Cannot read the frame back for %s.\n", path);
        free(pixels);
        return;
    }
    {
        FILE *fp = fopen(path, "wb");

        if (fp != NULL) {
            unsigned long y;

            fprintf(fp, "P6\n%lu %lu\n255\n", w, h);
            for (y = 0; y < h; y++)
                fwrite(pixels + (size_t)y * (size_t)w * 3u, 1,
                       (size_t)w * 3u, fp);
            fclose(fp);
        }
    }
    for (i = 0; i < (unsigned long)w * (unsigned long)h * 3u; i++) {
        int d = (int)pixels[i] - (int)expect[i];

        if (d < 0)
            d = -d;
        if (d > worst)
            worst = d;
        sum += (double)d;
        count += 1.0;
    }
    printf("dump mean abs diff %.2f, worst %d\n", sum / count, worst);
    free(pixels);
}

int bink_player_run(int argc, char **argv,
                    const BinkRendererBackend *backend)
{
    const char *path;
    bink b;
    SDL_Window *window = NULL;
    BinkRenderer *renderer = NULL;
    unsigned long w = 0;
    unsigned long h = 0;
    unsigned long frames = 0;
    double usf = 0.0;
    double frame_ms = 0.0;
    unsigned char track_mask = 0;
    unsigned char channels[7];
    unsigned char bitdepth[7];
    unsigned long rates[7];
    int enabled[7];
    BinkPlayback audio;
    char status = 0;
    int exit_code = 0;
    int quit = 0;
    int paused = 0;
    int device_paused = 0;
    int interp = 0;
    int has_next = 0;
    int cur = 0;
    unsigned long blocks_w = 0;
    unsigned long blocks_h = 0;
    unsigned long shown = 0;
    unsigned long test_frames = 0;
    long test_seek = -1;
    float t = 0.0f;
    double start_ms = 0.0;
    const char *dump_path = getenv("BINK_PLAYER_DUMP");
    const char *force_shader = getenv("BINK_PLAYER_SHADER");
    const char *force_t = getenv("BINK_PLAYER_T");
    const char *seek_env = getenv("BINK_PLAYER_TEST_SEEK");
    const char *frames_env;
    int i;

    memset(enabled, 0, sizeof(enabled));
    memset(&audio, 0, sizeof(audio));
    slot_rgb[0] = NULL;
    slot_rgb[1] = NULL;
    dump_enabled = dump_path != NULL;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s file.bik\n", argv[0]);
        return 1;
    }
    path = argv[1];
    frames_env = getenv("BINK_PLAYER_TEST_FRAMES");
    if (frames_env != NULL && frames_env[0] != '\0') {
        char *end = NULL;
        unsigned long value = strtoul(frames_env, &end, 10);

        if (end != frames_env && value > 0)
            test_frames = value;
    }
    if (force_shader != NULL && strcmp(force_shader, "interp") == 0)
        interp = 1;
    if (seek_env != NULL) {
        long value = strtol(seek_env, NULL, 10);

        if (value >= 0)
            test_seek = value;
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
    frame_ms = usf / 1000.0;

    bink_info_audio(b, &track_mask, channels, bitdepth, rates);
    if (bink_enable_video(b, 1) != 0) {
        fprintf(stderr, "Failed to enable video decoding.\n");
        bink_close(b);
        return 1;
    }
    for (i = 0; i < 7; i++) {
        if ((track_mask & (1u << i)) == 0 || bitdepth[i] != 16)
            continue;
        enabled[i] = 1;
        if (bink_enable_audio(b, (unsigned char)i, 1) != 0)
            enabled[i] = 0;
    }

#ifdef BINK_USE_SDL3
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        bink_close(b);
        return 1;
    }
    if (backend->prepare != NULL && backend->prepare() != 0) {
        fprintf(stderr, "Cannot prepare the renderer.\n");
        exit_code = 1;
        goto done;
    }
    window = SDL_CreateWindow(path, (int)w, (int)h, backend->window_flags);
#else
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        bink_close(b);
        return 1;
    }
    if (backend->prepare != NULL && backend->prepare() != 0) {
        fprintf(stderr, "Cannot prepare the renderer.\n");
        exit_code = 1;
        goto done;
    }
    window = SDL_CreateWindow(path, SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, (int)w, (int)h,
                              backend->window_flags);
#endif
    if (window == NULL) {
        fprintf(stderr, "Cannot create the window: %s\n", SDL_GetError());
        exit_code = 1;
        goto done;
    }
    renderer = backend->create(window, w, h);
    if (renderer == NULL) {
        fprintf(stderr, "Cannot create the %s renderer.\n", backend->name);
        exit_code = 1;
        goto done;
    }
    if (dump_enabled) {
        slot_rgb[0] = (unsigned char *)malloc((size_t)w * (size_t)h * 3u);
        slot_rgb[1] = (unsigned char *)malloc((size_t)w * (size_t)h * 3u);
        if (slot_rgb[0] == NULL || slot_rgb[1] == NULL) {
            fprintf(stderr, "Cannot allocate the frame buffers.\n");
            exit_code = 1;
            goto done;
        }
    }

    {
        int rate = 0;
        int ch = 0;

        for (i = 0; i < 7; i++) {
            if (!enabled[i])
                continue;
            rate = (int)rates[i];
            if (channels[i] > 1)
                ch = 2;
        }
        if (ch == 0)
            ch = 1;
        if (rate > 0) {
            if (bink_playback_audio_subsystem() != 0) {
                fprintf(stderr, "%s\n", SDL_GetError());
            } else if (bink_playback_audio_open(&audio, rate, ch) != 0) {
                fprintf(stderr, "%s\n", SDL_GetError());
            } else {
                device_paused = 1; /* SDL opens the device paused */
            }
        }
    }

    status = bink_first(b);
    if (status != BINK_MORE && status != BINK_LAST) {
        fprintf(stderr, "Failed to decode the first frame.\n");
        exit_code = 1;
        goto done;
    }
    upload_slot(renderer, backend, b, cur, w, h);
    bink_playback_publish(b, enabled, channels, rates, &audio, NULL);
    if (audio.ready)
        bink_playback_set_paused(&audio, 0, &device_paused);

    start_ms = (double)SDL_GetTicks();
    if (status == BINK_MORE) {
        status = bink_next(b);
        if (status == BINK_MORE || status == BINK_LAST) {
            upload_slot(renderer, backend, b, 1, w, h);
            has_next = 1;
            if (backend->upload_motion(renderer, b, &blocks_w,
                                              &blocks_h) != 0) {
                fprintf(stderr, "No motion field; interpolation off.\n");
                interp = 0;
            }
        }
    }

    printf("Opened file %s\nWidth: %lu\nHeight: %lu\nFrames: %lu\nFPS: %f\n",
           path, w, h, frames, 1000000.0 / usf);
    fflush(stdout);

    for (;;) {
        SDL_Event event;
        double now;
        int advanced = 0;
        int want_seek = 0;
        long seek_target = 0;

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
                bink_playback_set_paused(&audio, paused, &device_paused);
            } else if (key == BINK_KEY_M) {
                interp = !interp;
            } else if (key == SDLK_LEFT || key == SDLK_RIGHT) {
                unsigned long cur_frame = 0;
                unsigned long step;

                bink_info_all(b, &cur_frame, &frames, NULL);
                step = (unsigned long)(1000000.0 / usf + 0.5);
                if (step < 1)
                    step = 1;
                /* May be negative near the start; it is clamped below. */
                seek_target = (long)cur_frame +
                              (key == SDLK_LEFT ? -(long)step : (long)step);
                want_seek = 1;
            }
        }
        if (quit)
            break;

        if (test_seek >= 0) {
            seek_target = test_seek;
            want_seek = 1;
            test_seek = -1;
        }
        if (want_seek) {
            long target = seek_target;

            if (target < 0)
                target = 0;
            if (frames > 0 && target >= (long)frames)
                target = (long)frames - 1;
            if (bink_playback_seek(b, (unsigned long)target, &status, &audio,
                                   usf, enabled, channels, rates) == 0) {
                upload_slot(renderer, backend, b, cur, w, h);
                has_next = 0;
                blocks_w = 0;
                blocks_h = 0;
                start_ms = (double)SDL_GetTicks();
                t = 0.0f;
                if (status == BINK_MORE) {
                    char next_status = bink_next(b);

                    status = next_status;
                    if (next_status == BINK_MORE || next_status == BINK_LAST) {
                        upload_slot(renderer, backend, b, cur ^ 1, w, h);
                        has_next = 1;
                        if (backend->upload_motion(renderer, b, &blocks_w,
                                              &blocks_h) != 0) {
                            fprintf(stderr,
                                    "No motion field; interpolation off.\n");
                            interp = 0;
                        }
                    }
                }
            }
        }

        now = (double)SDL_GetTicks();
        if (audio.ready && has_next && !paused) {
            unsigned long frame_bytes = bink_frame_bytes(
                (unsigned long)audio.rate, audio.channels, usf);

            if (frame_bytes > 0 &&
                bink_playback_queued(&audio) > 2ul * frame_bytes)
                now = start_ms;
        }
        if (force_t != NULL) {
            t = (float)atof(force_t);
            if (t < 0.0f)
                t = 0.0f;
            if (t > 1.0f)
                t = 1.0f;
            if (!interp && dump_enabled)
                interp = 1;
        } else if (!paused && has_next) {
            double elapsed = now - start_ms;

            if (frame_ms > 0.0)
                t = (float)(elapsed / frame_ms);
            if (t >= 1.0f) {
                t = 0.0f;
                advanced = 1;
            }
        } else if (!has_next) {
            t = 0.0f;
        }

        {
            BinkRenderParams params;

            memset(&params, 0, sizeof(params));
            params.interp = interp;
            params.have_next = has_next;
            params.t = t;
            params.blocks_w = blocks_w;
            params.blocks_h = blocks_h;
            params.slot_cur = cur;
            params.capture = dump_path != NULL;
            if (backend->draw(renderer, &params) != 0) {
                fprintf(stderr, "Rendering failed.\n");
                exit_code = 1;
                break;
            }
        }

        if (dump_path != NULL) {
            const unsigned char *expect = slot_rgb[cur];

            if (force_t != NULL && atof(force_t) >= 0.5 && has_next)
                expect = slot_rgb[cur ^ 1];
            check_readback(renderer, backend, expect, dump_path, w, h);
            dump_path = NULL;
            if (force_t != NULL)
                break;
        }

        shown++;
        if (test_frames != 0 && shown >= test_frames)
            break;
        if (!has_next && status == BINK_LAST &&
            now - start_ms >= frame_ms)
            break;
        /* A frame that failed to decode ends playback: there is nothing
         * further to show, and waiting would hold the window open forever. */
        if (!has_next && status != BINK_MORE && status != BINK_LAST) {
            fprintf(stderr, "Decoding failed; stopping.\n");
            exit_code = 1;
            break;
        }

        if (advanced) {
            start_ms += frame_ms;
            if (start_ms < now - frame_ms)
                start_ms = now;
            cur ^= 1;
            bink_playback_publish(b, enabled, channels, rates, &audio, NULL);
            if (audio.ready)
                bink_playback_set_paused(&audio, 0, &device_paused);
            if (status == BINK_MORE) {
                status = bink_next(b);
                if (status == BINK_MORE || status == BINK_LAST) {
                    upload_slot(renderer, backend, b, cur ^ 1, w, h);
                    has_next = 1;
                    if (backend->upload_motion(renderer, b, &blocks_w,
                                              &blocks_h) != 0) {
                        fprintf(stderr, "No motion field; interpolation off.\n");
                        interp = 0;
                    }
                } else {
                    has_next = 0;
                }
            } else {
                has_next = 0;
            }
        }

        if (!advanced)
            wait_until(now + 4.0);
    }

done:
    backend->destroy(renderer);
    free(slot_rgb[0]);
    free(slot_rgb[1]);
    bink_playback_audio_close(&audio);
    bink_playback_shutdown();
    if (window != NULL)
        SDL_DestroyWindow(window);
    SDL_Quit();
    bink_close(b);
    return exit_code;
}
