# SDL player core + Metal renderer — design

Status: awaiting approval.

## Intent

Turn the OpenGL player into one shared SDL player core with pluggable
renderers, port the GL renderer onto it unchanged, and add a Metal
renderer for macOS. Vulkan becomes a third renderer later without
touching the loop.

## Why now

`bink_sdlgl_player.c` is one 500-line file that mixes the loop with GL
calls. A Metal player written the same way would duplicate the loop, and
Vulkan a third time. The Windows players already show the shape that
works: one main, a small renderer interface, one file per API.

## Goals

- One player loop, shared by OpenGL, Metal and (later) Vulkan.
- The GL player behaves exactly as it does now; the framebuffer dump is
  the regression gate.
- A Metal player with the same two shaders, keys, hooks and pacing.
- The dump check works for Metal too, so the shader can be verified
  against the decoder's own RGB.

## Non-goals

- Changing the Windows players, which already have this shape.
- Vulkan in this change.
- Audio or seek behaviour changes.

## Constraints

- The core stays C99. The Metal renderer is Objective-C (`bink_metal_renderer.m`),
  because Metal's API is.
- Metal shaders compile at run time from MSL strings, mirroring
  `D3DCompile` in the Windows players. No offline `metallib` step.
- No new dependency.
- The backend header must not include SDL, GL or Metal headers; only the
  core and the backends do.

## Structure

- `src/tools/bink_renderer.h` — the interface:
  ```c
  typedef struct BinkRenderer BinkRenderer;
  typedef struct {
      int interp;      /* 0 presentation, 1 interpolation */
      int have_next;
      float t;
      unsigned long blocks_w, blocks_h;
      int slot_cur;
  } BinkRenderParams;
  typedef struct {
      const char *name;            /* "opengl", "metal" */
      unsigned int window_flags;   /* SDL_WINDOW_OPENGL or SDL_WINDOW_METAL */
      BinkRenderer *(*create)(void *window, unsigned long w, unsigned long h);
      int (*upload)(BinkRenderer *, bink, int slot);
      int (*upload_motion)(BinkRenderer *, bink);
      int (*draw)(BinkRenderer *, const BinkRenderParams *);
      /* Read the last frame back as packed RGB24, for the dump check. */
      int (*readback)(BinkRenderer *, unsigned char *rgb, unsigned long w,
                      unsigned long h);
      void (*destroy)(BinkRenderer *);
  } BinkRendererBackend;
  ```
  The renderer struct is defined by each backend, so exactly one is
  linked per player.
- `src/tools/bink_sdl_player_core.{c,h}` — `bink_player_run(argc, argv,
  backend)`: window, file, audio, frame pair, clock, keys, hooks, dump
  comparison, cleanup. The four hook variables become
  `BINK_PLAYER_TEST_FRAMES`, `BINK_PLAYER_SHADER`, `BINK_PLAYER_T`,
  `BINK_PLAYER_DUMP`, `BINK_PLAYER_TEST_SEEK`.
- `src/tools/bink_gl_renderer.c` — the current GL code, unchanged apart
  from taking the SDL window and exposing the interface. Includes the
  context creation/destroy and the GL readback.
- `src/tools/bink_metal_renderer.m` — Metal device, queue, layer from
  `SDL_Metal_CreateView`/`SDL_Metal_GetLayer`, two pipeline states from
  runtime-compiled MSL, `r8Unorm` planes and `rg8Unorm` motion, linear
  and nearest samplers, offscreen readback for the dump.
- `src/tools/bink_sdlgl_player.c` and `bink_sdlmetal_player.c` — each a
  short main calling `bink_player_run` with its backend.

## Metal details

- The vertex function generates a full-screen triangle pair from
  `vertex_id`; there is no vertex buffer.
- Uniforms go through `setFragmentBytes`, not a buffer.
- The layer's `drawableSize` follows the window; the render pass clears
  and draws, and the drawable presents.
- Readback renders the same frame into an offscreen `bgra8Unorm` texture
  with shared storage, then `getBytes` after `waitUntilCompleted`, so
  the dump path exercises the real pipelines.

## Tests

- **Regression gate:** `BINK_PLAYER_DUMP` on the GL player still reports
  a mean absolute difference of about 0.16 against `bink_get_video`.
  This runs before and after the refactor.
- **Metal:** the same dump through the Metal renderer, same comparison,
  expecting the same order of difference.
- **Keys and hooks:** the existing `TEST_SEEK` and `TEST_FRAMES` runs
  still terminate.
- **Suite:** 17/17 unchanged.

## Risks

- The refactor touches a working player; the dump gate is what makes it
  safe, and it runs first.
- Metal readback needs the offscreen texture to be `MTLStorageModeShared`
  on Apple Silicon for `getBytes` to work; if it is not, the dump path
  reports that rather than silently passing.
- `SDL_Metal_CreateView` requires the window to be created with
  `SDL_WINDOW_METAL`; the backend descriptor carries the flag so the
  core creates the right window.
