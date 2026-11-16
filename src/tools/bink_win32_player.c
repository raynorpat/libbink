/*
 * libbink - Windows Direct3D playback example
 *
 * Two executables share this source: bink_d3d9_player and
 * bink_d3d11_player. Both present the decoded planes through a pixel
 * shader and interpolate between frames with the decoder's motion field.
 *
 * Usage:
 *   bink_d3d9_player file.bik
 *   bink_d3d11_player file.bik
 *
 * Keys: Esc/q quit, Space pauses, Left/Right seek one second,
 *       M toggles interpolation.
 *
 * Test hooks: BINK_D3D_TEST_FRAMES=N exits after N frames;
 *             BINK_D3D_SHADER=present|interp picks the starting shader.
 */

#include <windows.h>

#include "bink.h"
#include "tools/bink_mix.h"
#include "tools/bink_playback.h"
#include "tools/bink_d3d_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef BINK_D3D11
#define RENDERER_CREATE bink_d3d11_renderer_create
#define RENDERER_NAME "d3d11"
#else
#define RENDERER_CREATE bink_d3d9_renderer_create
#define RENDERER_NAME "d3d9"
#endif

static volatile LONG pending_key;

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam)
{
    if (message == WM_CLOSE || message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    if (message == WM_KEYDOWN) {
        InterlockedExchange(&pending_key, (LONG)wparam);
        return 0;
    }
    return DefWindowProc(hwnd, message, wparam, lparam);
}

static int key_is(LONG key, int vk)
{
    return key == (LONG)vk;
}

int main(int argc, char **argv)
{
    const char *path;
    bink b;
    HWND hwnd = NULL;
    BinkD3DRenderer *renderer = NULL;
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
    int slot_cur = 0;
    unsigned long blocks_w = 0;
    unsigned long blocks_h = 0;
    unsigned long shown = 0;
    unsigned long test_frames = 0;
    float t = 0.0f;
    ULONGLONG start_ms = 0;
    const char *test_env;
    const char *shader_env;
    WNDCLASSA wc;
    RECT rect;
    int i;

    memset(enabled, 0, sizeof(enabled));
    memset(&audio, 0, sizeof(audio));
    if (argc != 2) {
        fprintf(stderr, "Usage: %s file.bik\n", argv[0]);
        return 1;
    }
    path = argv[1];

    test_env = getenv("BINK_D3D_TEST_FRAMES");
    if (test_env != NULL && test_env[0] != '\0') {
        char *end = NULL;
        unsigned long value = strtoul(test_env, &end, 10);

        if (end != test_env && value > 0)
            test_frames = value;
    }
    shader_env = getenv("BINK_D3D_SHADER");
    if (shader_env != NULL && strcmp(shader_env, "interp") == 0)
        interp = 1;

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

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "libbink_d3d_player";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    if (RegisterClassA(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        fprintf(stderr, "Cannot register the window class.\n");
        bink_close(b);
        return 1;
    }
    rect.left = 0;
    rect.top = 0;
    rect.right = (LONG)w;
    rect.bottom = (LONG)h;
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA("libbink_d3d_player", path, WS_OVERLAPPEDWINDOW,
                         CW_USEDEFAULT, CW_USEDEFAULT,
                         rect.right - rect.left, rect.bottom - rect.top, NULL,
                         NULL, wc.hInstance, NULL);
    if (hwnd == NULL) {
        fprintf(stderr, "Cannot create the window.\n");
        bink_close(b);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);

    renderer = RENDERER_CREATE(hwnd, w, h);
    if (renderer == NULL) {
        fprintf(stderr, "Cannot create the %s renderer.\n", RENDERER_NAME);
        DestroyWindow(hwnd);
        bink_close(b);
        return 1;
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
                fprintf(stderr, "Cannot start the audio subsystem.\n");
            } else if (bink_playback_audio_open(&audio, rate, ch) != 0) {
                fprintf(stderr, "Cannot open the audio device.\n");
            } else {
                device_paused = 1; /* waveOut opens ready; paused state is ours */
            }
        }
    }

    status = bink_first(b);
    if (status != BINK_MORE && status != BINK_LAST) {
        fprintf(stderr, "Failed to decode the first frame.\n");
        exit_code = 1;
        goto done;
    }
    bink_d3d_renderer_upload(renderer, b, 0);
    bink_playback_publish(b, enabled, channels, rates, &audio, NULL);
    if (audio.ready)
        bink_playback_set_paused(&audio, 0, &device_paused);

    start_ms = GetTickCount64();
    if (status == BINK_MORE) {
        status = bink_next(b);
        if (status == BINK_MORE || status == BINK_LAST) {
            bink_d3d_renderer_upload(renderer, b, 1);
            has_next = 1;
            if (bink_d3d_renderer_upload_motion(renderer, b) != 0) {
                fprintf(stderr, "No motion field; interpolation off.\n");
                interp = 0;
            }
        }
    }

    printf("Opened file %s\nWidth: %lu\nHeight: %lu\nFrames: %lu\nFPS: %f\n",
           path, w, h, frames, 1000000.0 / usf);
    fflush(stdout);

    while (!quit) {
        MSG message;
        BinkD3DDraw draw;
        ULONGLONG now;

        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quit = 1;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        if (quit)
            break;

        {
            LONG key = InterlockedExchange(&pending_key, 0);

            if (key != 0) {
                if (key_is(key, VK_ESCAPE) || key_is(key, 'Q')) {
                    quit = 1;
                    continue;
                }
                if (key_is(key, VK_SPACE)) {
                    paused = !paused;
                    bink_playback_set_paused(&audio, paused, &device_paused);
                } else if (key_is(key, 'M')) {
                    interp = !interp;
                    {
                        char title[512];

                        snprintf(title, sizeof(title),
                                 "bink_%s_player - %s - %s", RENDERER_NAME, path,
                                 interp ? "interpolation" : "presentation");
                        SetWindowTextA(hwnd, title);
                    }
                } else if (key_is(key, VK_LEFT) || key_is(key, VK_RIGHT)) {
                    unsigned long cur_frame = 0;
                    unsigned long step;
                    long target;

                    bink_info_all(b, &cur_frame, &frames, NULL);
                    step = (unsigned long)(1000000.0 / usf + 0.5);
                    if (step < 1)
                        step = 1;
                    target = (long)cur_frame +
                             (key_is(key, VK_LEFT) ? -(long)step : (long)step);
                    if (target < 0)
                        target = 0;
                    if (frames > 0 && target >= (long)frames)
                        target = (long)frames - 1;
                    if (bink_playback_seek(b, (unsigned long)target, &status,
                                           &audio, usf, enabled, channels,
                                           rates) == 0) {
                        bink_d3d_renderer_upload(renderer, b, cur);
                        has_next = 0;
                        blocks_w = 0;
                        blocks_h = 0;
                        start_ms = GetTickCount64();
                        t = 0.0f;
                        if (status == BINK_MORE) {
                            char next_status = bink_next(b);

                            if (next_status == BINK_MORE ||
                                next_status == BINK_LAST) {
                                status = next_status;
                                bink_d3d_renderer_upload(renderer, b, cur ^ 1);
                                has_next = 1;
                                if (bink_d3d_renderer_upload_motion(renderer,
                                                                    b) != 0) {
                                    fprintf(stderr,
                                            "No motion field; interpolation "
                                            "off.\n");
                                    interp = 0;
                                }
                            }
                        }
                    }
                }
            }
        }

        now = GetTickCount64();
        if (audio.ready && has_next && !paused) {
            /* The queue only vetoes: hold the picture while the device is
             * more than two frames behind. */
            unsigned long frame_bytes = bink_frame_bytes(
                (unsigned long)audio.rate, audio.channels, usf);

            if (frame_bytes > 0 &&
                bink_playback_queued(&audio) > 2ul * frame_bytes)
                now = start_ms;
        }
        if (!paused && has_next) {
            double elapsed = (double)(now - start_ms);

            if (frame_ms > 0.0)
                t = (float)(elapsed / frame_ms);
            if (t >= 1.0f) {
                t = 0.0f;
                start_ms += (ULONGLONG)(frame_ms + 0.5);
                if ((double)(now - start_ms) > frame_ms)
                    start_ms = now;
                cur ^= 1;
                slot_cur = cur;
                bink_playback_publish(b, enabled, channels, rates, &audio,
                                      NULL);
                if (audio.ready)
                    bink_playback_set_paused(&audio, 0, &device_paused);
                if (status == BINK_MORE) {
                    status = bink_next(b);
                    if (status == BINK_MORE || status == BINK_LAST) {
                        bink_d3d_renderer_upload(renderer, b, cur ^ 1);
                        has_next = 1;
                        if (bink_d3d_renderer_upload_motion(renderer, b) != 0) {
                            fprintf(stderr, "No motion field; interpolation "
                                            "off.\n");
                            interp = 0;
                        }
                    } else {
                        has_next = 0;
                    }
                } else {
                    has_next = 0;
                }
            }
        } else if (!has_next) {
            t = 0.0f;
        }

        memset(&draw, 0, sizeof(draw));
        draw.interp = interp;
        draw.have_next = has_next;
        draw.t = t;
        draw.blocks_w = blocks_w;
        draw.blocks_h = blocks_h;
        draw.slot_cur = slot_cur;
        if (bink_d3d_renderer_draw(renderer, &draw) != 0) {
            fprintf(stderr, "Rendering failed.\n");
            exit_code = 1;
            break;
        }
        shown++;
        if (test_frames != 0 && shown >= test_frames)
            break;
        if (!has_next && status == BINK_LAST &&
            (double)(now - start_ms) >= frame_ms)
            break;
    }

done:
    bink_playback_audio_close(&audio);
    bink_playback_shutdown();
    bink_d3d_renderer_destroy(renderer);
    if (hwnd != NULL && !quit)
        DestroyWindow(hwnd);
    bink_close(b);
    return exit_code;
}
