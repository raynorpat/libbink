# SDL player core + Metal renderer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the OpenGL player onto a shared SDL loop behind a
renderer interface, prove it is inert, then add a Metal player on the
same loop.

**Architecture:** `bink_renderer.h` defines the backend interface;
`bink_sdl_player_core.c` owns window, audio, clock, keys and hooks; each
player is a short `main` plus a renderer file (`bink_gl_renderer.c`,
`bink_metal_renderer.m`).

**Tech Stack:** C99 core, one Objective-C translation unit for Metal,
SDL2/SDL3, OpenGL 3.2, Metal, CMake.

**Spec:** `docs/superpowers/specs/2026-11-16-sdl-core-metal-design.md`

## Global Constraints

- The GL player must behave exactly as before; the dump check is the
  gate and runs before and after the move.
- `bink_renderer.h` includes no SDL, GL or Metal headers.
- Audio, seek and pacing code does not change.
- Metal shaders compile at run time from MSL strings.
- No new dependency; `enable_language(OBJC)` only on Apple.

## Review Focus

- The GL context: created after the window, used only while it is
  current, destroyed before the window.
- The dump: readback must produce exactly the plane size, top-down,
  whatever the drawable size is.
- The first frame and a seek: the pair must be seeded so the
  interpolation has two textures.
- A window resize or a Retina scale change: the drawable size drives the
  viewport, not the window size.
- Metal on a texture that cannot be read back: report, do not compare
  garbage.

---

### Task 1: Renderer interface, shared core, GL ported

**Files:**
- Create: `src/tools/bink_renderer.h`, `src/tools/bink_sdl_player_core.c`,
  `src/tools/bink_sdl_player_core.h`, `src/tools/bink_gl_renderer.c`
- Modify: `src/tools/bink_sdlgl_player.c`, `CMakeLists.txt`

**Interfaces:**
- Produces `BinkRenderParams`, `BinkRendererBackend` and
  `int bink_player_run(int argc, char **argv,
  const BinkRendererBackend *backend);` as the spec defines them.

- [ ] **Step 1: Record the baseline**

Run: `BINK_SDLGL_TEST_FRAMES=2 BINK_SDLGL_DUMP=/tmp/base.ppm ./build/bink_sdlgl_player <clip>`
Expected: `dump mean abs diff` about 0.16. Write the number down.

- [ ] **Step 2: Write the interface**

`bink_renderer.h` with the structs and function pointers from the spec,
including `readback` returning packed RGB24 at the plane size.

- [ ] **Step 3: Move the loop**

`bink_sdl_player_core.c` takes the loop, window, audio, hooks and dump
comparison out of `bink_sdlgl_player.c` unchanged apart from calling
`backend->draw`/`readback`, renaming the hooks to `BINK_PLAYER_*`, and
taking `window_flags` from the backend when creating the window.

- [ ] **Step 4: Move the GL code**

`bink_gl_renderer.c` holds the loader, programs, textures, upload, draw
and a `glReadPixels` readback that nearest-samples the drawable to the
plane size. `bink_sdlgl_player.c` becomes `main` with a backend table.

- [ ] **Step 5: Prove nothing changed**

Run: `cmake --build build && ctest --test-dir build`
Expected: 100%.

Run: `BINK_PLAYER_TEST_FRAMES=2 BINK_PLAYER_DUMP=/tmp/after.ppm ./build/bink_sdlgl_player <clip>`
Expected: the same mean difference as Step 1 (about 0.16).

Run: `BINK_PLAYER_TEST_SEEK=4 BINK_PLAYER_TEST_FRAMES=7 ./build/bink_sdlgl_player <clip>`
Expected: exits 0.

- [ ] **Step 6: Commit**

```bash
git add src/tools/bink_renderer.h src/tools/bink_sdl_player_core.c \
        src/tools/bink_sdl_player_core.h src/tools/bink_gl_renderer.c \
        src/tools/bink_sdlgl_player.c CMakeLists.txt
git commit -m "tools: move the SDL player onto a shared renderer interface"
```

---

### Task 2: Metal renderer and player

**Files:**
- Create: `src/tools/bink_metal_renderer.m`,
  `src/tools/bink_sdlmetal_player.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add the language and target**

`enable_language(OBJC)` under `if(APPLE)`. Add `bink_sdlmetal_player`
with the core, the Metal renderer, playback, mix and the SDL link, plus
`-framework Metal -framework QuartzCore -framework Foundation`.

- [ ] **Step 2: Renderer**

Device and queue; `SDL_Metal_CreateView` on the window and
`SDL_Metal_GetLayer` for the `CAMetalLayer`; MSL compiled at run time
with `newLibraryWithSource`; present and interpolation pipelines; `r8Unorm`
planes and `rg8Unorm` motion; linear and nearest samplers; `Params` via
`setFragmentBytes`; a full-screen triangle strip generated from
`vertex_id`.

- [ ] **Step 3: Readback**

Render the same pass into an offscreen `bgra8Unorm` texture with shared
storage, `waitUntilCompleted`, `getBytes`, and convert to RGB24 at the
plane size. Report if the texture cannot be read.

- [ ] **Step 4: Player main**

`bink_sdlmetal_player.c` is the backend table (`SDL_WINDOW_METAL`,
`bink_metal_renderer_create`) and `bink_player_run`.

- [ ] **Step 5: Verify**

Run: `cmake --build build && ctest --test-dir build`
Expected: 100%.

Run: `BINK_PLAYER_TEST_FRAMES=2 BINK_PLAYER_DUMP=/tmp/metal.ppm ./build/bink_sdlmetal_player <clip>`
Expected: `dump mean abs diff` in the same range as the GL player.

Run the player without hooks on a clip with motion, toggle `M`, seek.

- [ ] **Step 6: Commit**

```bash
git add src/tools/bink_metal_renderer.m src/tools/bink_sdlmetal_player.c \
        CMakeLists.txt
git commit -m "tools: add a Metal player for macOS"
```

---

### Task 3: Documentation

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Describe the two SDL players**

Note that `bink_sdlgl_player` and `bink_sdlmetal_player` share one loop,
list the keys and the hooks, and say Metal is macOS only.

- [ ] **Step 2: Build and run the suite**

Run: `cmake --build build && ctest --test-dir build`
Expected: 100%.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: describe the Metal player"
```

---

## Self-Review

- **Spec coverage:** interface and core → Task 1; Metal → Task 2; docs →
  Task 3.
- **Review Focus:** context lifetime → Task 1 step 4; readback size → Task
  1 step 4 and Task 2 step 3; pair seeding and seek → core (unchanged,
  covered by the seek run); drawable size → the GL renderer keeps using
  it; unreadable Metal texture → Task 2 step 3.
- **Type consistency:** `BinkRendererBackend`, `BinkRenderParams` and
  `bink_player_run` are used unchanged from Task 1 in Task 2.
- **Proportion:** three tasks; the code is the product and the plan pins
  interfaces, not bodies.
