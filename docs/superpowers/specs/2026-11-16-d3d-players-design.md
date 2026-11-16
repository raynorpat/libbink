# Windows D3D9 / D3D11 players — design

Status: awaiting approval.

## Intent

Two Windows players with the same behaviour as `bink_sdlgl_player`: a
YUV-to-RGB presentation shader, and a second shader that interpolates
between decoded frames using the decoder's motion field, toggled with
`M`. One uses Direct3D 9, one Direct3D 11.

## Why raw Win32 instead of SDL

mingw-w64 ships `d3d9.h`, `d3d11.h`, `d3dcompiler.h` and their import
libraries, but there is no SDL for mingw and Homebrew's SDL is macOS
only. A raw Win32 player therefore compiles and links from this machine,
which is the only verification available. An SDL-based one could not be
built at all until it reached a Windows machine.

## Goals

- `bink_d3d9_player` and `bink_d3d11_player`, built by CMake on Windows
  and cross-compilable with mingw here.
- The same two shaders, keys, pacing and audio behaviour as the GL
  player.
- Audio, seek and timing shared through the existing playback module,
  with the device layer split per backend as requested.

## Non-goals

- Exclusive fullscreen, vsync toggles, or device-lost handling beyond a
  reset attempt.
- Dump/readback test hooks. They cannot be exercised here, and the shader
  math is already verified through the GL player.
- Replacing or changing the SDL players.

## Anti-goals

- A Windows player that builds but silently shows a black window because
  a shader failed: every device, shader and texture step reports and
  exits non-zero.

## Constraints

- C99, no new dependency beyond the Windows SDK and `bink_mix`.
- No change to `src/bink.h` or decoder behaviour.
- Shaders are compiled at run time from strings in the source, with
  `D3DCompile` from `d3dcompiler`, so there is no offline shader build
  step and no D3DX dependency (absent from mingw and deprecated).

## Playback split

The current `bink_playback.c` mixes two layers. Split it:

- `src/tools/bink_playback.c` — stays backend-neutral: `publish` (mix
  and queue), `seek`, `shutdown`, and the `BinkPlayback` state
  (`ready`, `rate`, `channels`, plus an opaque `void *device`).
- `src/tools/bink_playback_sdl.c` — the SDL device: open, close, clear,
  queued, queue, set_paused, subsystem init. Holds its
  `SDL_AudioStream`/`SDL_AudioDeviceID` in a heap context behind
  `device`.
- `src/tools/bink_playback_win32.c` — the same functions over `waveOut`:
  a small ring of `WAVEHDR`s, each filled from the queue and recycled
  once `WHDR_DONE` is set; `queued` is the sum of the bytes still
  playing.

`bink_playback.h` keeps one `BinkPlayback` type with no SDL include, so
the Windows players do not need SDL headers. The SDL players include the
same header and link the SDL backend.

## Player structure

- `src/tools/bink_win32_player.c` — `main`, window and message pump,
  file opening, audio publish/seek, the frame-pair clock, keys
  (Esc/q, Space, Left/Right, `M`), and the frame counter hook. It owns
  everything that is not GPU work.
- `src/tools/bink_d3d_renderer.h` — the interface the player calls:
  `init(hwnd, w, h)`, `upload(hwnd, bink, slot)`, `upload_motion(bink)`,
  `draw(slot_cur, slot_next, t, interp)`, `present()`, `shutdown()`.
- `src/tools/bink_d3d9_renderer.c` — D3D9 9Ex-free device, `D3DFMT_L8`
  planes, `D3DFMT_A8L8` motion, pre-transformed quad, `ps_3_0` shaders.
- `src/tools/bink_d3d11_renderer.c` — device, swap chain, RTV,
  `R8_UNORM` planes, `RG8_UNORM` motion, full-screen triangle, `vs_4_0`
  and `ps_4_0`, constant buffer, linear and point samplers.

Both renderers upload with a staging copy only when the row pitch
differs from the plane width; otherwise they upload the plane directly.

## Shader math

Identical to the GLSL, in HLSL:

```
b     = (int2)(uv * luma_size / 8)
mv    = (motion.Load(int3(b, 0)).rg * 255 - 128) / luma_size
yuv0  = sample(frame0, uv + t * mv)
yuv1  = sample(frame1, uv - (1 - t) * mv)
yuv   = lerp(yuv0, yuv1, t)
rgb   = mul(matrix, yuv) + offset
```

D3D9 lacks `texelFetch`, so the motion texture uses a point sampler and
the block index is snapped to its texel centre before sampling. Chroma
uses the same normalised delta, because a chroma texel covers 2x2 luma
pixels.

## Keys and hooks

The same keys as the GL player. `BINK_SDLGL_*` becomes
`BINK_D3D_TEST_FRAMES` and `BINK_D3D_SHADER`, so a Windows user can run
the player headless-ish (frames and exit) and start in either shader.

## Build

- CMake gains, under `if(WIN32)`, both executables linking the renderer,
  `bink_playback.c`, `bink_playback_win32.c` and `bink_mix.c`, plus
  `d3d9`, `d3d11`, `dxguid`, `d3dcompiler`, `winmm`.
- A mingw toolchain file (`cmake/mingw-w64.cmake`) plus a documented
  command so the build can be reproduced here:
  `cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DBINK_BUILD_TOOLS=ON`.
- The SDL players stay off in a cross build because no SDL for mingw.

## Tests

- **Build gate:** both players compile and link with
  `x86_64-w64-mingw32-gcc` from this machine.
- **Existing suite:** unchanged; the playback split must keep
  `bink_sdl_player` and `bink_sdlgl_player` building and their runs
  unchanged.
- **Runtime:** the human runs both on Windows, toggles `M`, seeks, and
  confirms the picture and sound.

## Risks

- None of this runs here. The gate is compile and link, so a wrong
  constant buffer layout or sampler state will surface only on Windows.
- D3D9 `L8` samples as `(L,L,L,1)`; the shaders must read `.r`, and
  `A8L8` supplies the second motion byte in `.a`.
- `ps_3_0` is required on the D3D9 side (seven texture fetches with
  computed coordinates), which excludes pre-2004 hardware. Acceptable
  for a test player.
