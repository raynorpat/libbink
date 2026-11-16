# GLSL Player Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `bink_gl_player`, an SDL2/SDL3 + OpenGL player whose two
GLSL shaders either present decoded YUV or reconstruct in-between frames
from the decoder's per-block motion vectors.

**Architecture:** The decoder records a per-block luma motion field and
exposes it through one read-only accessor. A new tool shares the audio
device and bink-side playback glue with the existing player through a
small module, then uploads one or two YUV triples plus a motion texture
and renders through a GL 3.2 core context whose entry points all come
from `SDL_GL_GetProcAddress`.

**Tech Stack:** C99, SDL2/SDL3, OpenGL 3.2 core, GLSL 150, CMake.

**Spec:** `docs/superpowers/specs/2026-11-16-glsl-player-design.md`

## Global Constraints

- The decoder's output must not change. The motion field is additive.
- GL 3.2 entry points are resolved through `SDL_GL_GetProcAddress` on
  every platform; never link `OpenGL::GL` and never rely on static
  prototypes.
- GLSL 150 (`#version 150`), core profile 3.2.
- `bink_get_*` accessors are read-only.
- The existing player keeps working; its audio moves, its behaviour does
  not.
- No new dependency.

## Review Focus

- A plane whose size is not a multiple of 8 (17x9 exists in the suite):
  the shader's block lookup must not shift by half a block, and the
  decoder's field must cover `ceil(w/8) x ceil(h/8)` blocks.
- A frame with no motion at all (keyframe, or a still): the interpolation
  shader must crossfade, not divide by zero or sample garbage.
- A seek while interpolating: the held frame pair must be dropped, or the
  picture blends across the seek.
- Audio present but the display hitting a deadline early or late: the
  pair must not advance more than one frame per deadline.
- A driver missing a GL 3.2 symbol: the player must say which symbol and
  exit non-zero rather than crash.

---

### Task 1: Decoder motion field and accessor

**Files:**
- Modify: `src/bink_video.h` (state), `src/bink_video.c` (capture),
  `src/bink.h` and `src/bink.c` (accessor)
- Test: `tests/test_encoder.c`

**Interfaces:**
- Produces:
  ```c
  int bink_get_motion(const bink object, unsigned long *blocks_w,
                      unsigned long *blocks_h, const signed char **mv_x,
                      const signed char **mv_y);
  ```
  Returns 1 when video is enabled and the field exists, 0 otherwise; the
  four out-parameters are optional and may be NULL.

- [ ] **Step 1: Write the failing test**

Add `test_motion_field` beside `test_skip_and_motion`, reusing its
`fill_shift` clip (a gradient shifted by (3, 2) on the second frame):

```c
static int test_motion_field(void)
{
    /* encode two frames, shift (3,2), keyframe 0, motion range 8 */
    /* assert: bink_get_motion returns 1, blocks_w/blocks_h match the
       encoder's grid, every vector on frame 0 is (0,0), and frame 1 has at
       least one vector exactly (3,2) */
}
```

Register it in `main` after `test_skip_and_motion`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build && ./build/tests/test_encoder`
Expected: compile error on `bink_get_motion`.

- [ ] **Step 3: Record the field**

In `src/bink_video.h`, add to `BinkVideo`:

```c
    signed char *mv_x; /* bw * bh, one per 8x8 luma block */
    signed char *mv_y;
```

Allocate both in `bink_video_init` after `bw`/`bh` are known and free
them in `bink_video_uninit`. Clear them in `bink_video_decode` before
the planes are decoded. In `src/bink_video.c`, give `bink_put_pixels` two
extra out-parameters for the offsets it read (`int *xoff_out, int
*yoff_out`) and have `decode_plane` store them for `plane == 0` in the
MOTION, RESIDUE and INTER arms. Blocks of every other type keep the
cleared (0,0).

- [ ] **Step 4: Add the accessor**

In `src/bink.c`, following the existing accessors' style:

```c
int bink_get_motion(const bink object, unsigned long *blocks_w,
                    unsigned long *blocks_h, const signed char **mv_x,
                    const signed char **mv_y)
```

Return 0 when the object or its video is missing or video is disabled.
Declare it in `src/bink.h` with the comment from the spec.

- [ ] **Step 5: Run the tests**

Run: `cmake --build build && ./build/tests/test_encoder && ctest --test-dir build`
Expected: the new test passes, everything else unchanged.

- [ ] **Step 6: Commit**

```bash
git add src/bink.h src/bink.c src/bink_video.c src/bink_video.h tests/test_encoder.c
git commit -m "decoder: expose the per-block motion field"
```

---

### Task 2: Shared playback module

**Files:**
- Create: `src/tools/bink_playback.h`, `src/tools/bink_playback.c`
- Modify: `src/tools/bink_sdl_player.c`, `CMakeLists.txt`

**Interfaces:**
- Produces (exact names; move the bodies verbatim from
  `bink_sdl_player.c` and rename `PlayerAudio` to `BinkPlayback`):
  ```c
  typedef struct { ... } BinkPlayback;
  int bink_playback_audio_open(BinkPlayback *p, int rate, int channels);
  void bink_playback_audio_close(BinkPlayback *p);
  void bink_playback_audio_clear(BinkPlayback *p);
  unsigned long bink_playback_queued(const BinkPlayback *p);
  void bink_playback_queue(BinkPlayback *p, const void *data, unsigned long bytes);
  void bink_playback_set_paused(BinkPlayback *p, int pause_on, int *device_paused);
  int bink_playback_publish(bink object, const int enabled[7], BinkPlayback *p,
                            unsigned long *audio_bytes);
  int bink_playback_seek(bink object, unsigned long target, char *status,
                         BinkPlayback *p, double usf, const int enabled[7],
                         unsigned char channels[7], unsigned long rates[7]);
  ```

- [ ] **Step 1: Move the code**

Create the two files and move the audio device, `publish_audio`,
`mix_current`, `append_pcm` and `seek_to_frame` into them unchanged apart
from the type rename. `bink_sdl_player.c` keeps only its loop, window,
texture, event handling and `present`/`wait_until`, and calls the shared
functions.

- [ ] **Step 2: Wire the build**

Add `src/tools/bink_playback.c` and `src/tools/bink_mix.c` to the SDL
player target's sources, and add `src/tools` to its include path if it
is not already there.

- [ ] **Step 3: Verify behaviour is unchanged**

Run: `cmake --build build && ctest --test-dir build`
Expected: 100%, including the SDL player test that runs it headless with
`BINK_SDL_TEST_FRAMES`.

Run the player on a clip with audio and confirm playback, pause and seek
are unchanged.

- [ ] **Step 4: Commit**

```bash
git add src/tools/bink_playback.c src/tools/bink_playback.h \
        src/tools/bink_sdl_player.c CMakeLists.txt
git commit -m "tools: share the playback audio and seek code"
```

---

### Task 3: GL player with the presentation shader

**Files:**
- Create: `src/tools/bink_gl_player.c`, `src/tools/bink_gl_shaders.h`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `BinkPlayback` from Task 2, `bink_get_motion` from Task 1
  (not used until Task 4).
- Produces: a `bink_gl_player` executable; shader source strings
  `bink_gl_vs`, `bink_gl_fs_present`, `bink_gl_fs_interpolate` (the last
  filled in by Task 4).

- [ ] **Step 1: Add the target**

In `CMakeLists.txt`, inside the existing SDL block, add
`add_executable(bink_gl_player src/tools/bink_gl_player.c
src/tools/bink_playback.c src/tools/bink_mix.c)` with the same compile
settings and SDL linkage as `bink_sdl_player`, plus the deprecation
warning suppressed for this target only on Apple. No OpenGL link.

- [ ] **Step 2: Write the GL skeleton**

In `bink_gl_player.c`: open the file with `bink_open_file`, enable video,
print the same summary the SDL player does, create a window with
`SDL_WINDOW_OPENGL`, set the context attributes (3.2 core, double
buffer), create the context, resolve the entry points used (`glGen*`,
`glBind*`, `glBufferData`, `glVertexAttribPointer`, `glCreateShader`,
`glShaderSource`, `glCompileShader`, `glGetShaderiv`,
`glGetShaderInfoLog`, `glCreateProgram`, `glAttachShader`,
`glBindAttribLocation`, `glLinkProgram`, `glUseProgram`,
`glGetUniformLocation`, `glUniform*`, `glActiveTexture`, `glTexImage2D`,
`glTexParameteri`, `glPixelStorei`, `glDrawArrays`, `glViewport`,
`glClearColor`, `glClear`, `glEnable`, `glDisable`) through
`SDL_GL_GetProcAddress`, failing with the symbol name when one is NULL.

- [ ] **Step 3: Presentation shader and textures**

Compile `bink_gl_fs_present`: sample `u_y/u_u/u_v`, apply
`mat3 u_yuv_to_rgb` and `vec3 u_offset`, clamp. Create three `GL_R8`
textures at plane sizes, upload with `GL_UNPACK_ROW_LENGTH` set to each
plane's stride, `GL_LINEAR` and `GL_CLAMP_TO_EDGE`. Draw a full-screen
triangle pair with uv 0..1.

- [ ] **Step 4: Loop, keys, pacing**

Loop like the SDL player: decode, publish audio through
`BinkPlayback`, upload the planes, draw, wait for the frame deadline
(wall clock, or the audio deadline when audio is ready). Keys: Esc/q
quit, Space pause, Left/Right seek via `bink_playback_seek`. Title:
`bink_gl_player - presentation`.

- [ ] **Step 5: Build and run**

Run: `cmake -S . -B build -DBINK_BUILD_TOOLS=ON && cmake --build build`
Expected: `bink_gl_player` builds with no warnings from our code.

Run: `./build/bink_gl_player <clip>` and confirm the picture matches the
SDL player's frames in order.

- [ ] **Step 6: Commit**

```bash
git add src/tools/bink_gl_player.c src/tools/bink_gl_shaders.h CMakeLists.txt
git commit -m "tools: add an OpenGL player with a presentation shader"
```

---

### Task 4: Motion-compensated interpolation shader

**Files:**
- Modify: `src/tools/bink_gl_player.c`, `src/tools/bink_gl_shaders.h`

**Interfaces:**
- Consumes: `bink_get_motion`.
- Produces: nothing outside the tool.

- [ ] **Step 1: Hold two frames**

Add a second YUV triple and an `RG8` motion texture of size
`blocks_w x blocks_h`. After decoding a frame, if it is not a keyframe,
copy its motion field into the texture (`mv + 128`, the enums already
fit in a byte). Keep the previous triple and its motion texture; on the
first frame, or after a seek, duplicate the current frame into both so
`s` has a defined pair.

- [ ] **Step 2: Compile the second shader**

`bink_gl_fs_interpolate` implements the spec's formula, with
`u_plane_size`, `u_block_scale` (8.0 luma, 4.0 chroma) and `u_t`.
Sample the luma and both chroma planes from both triples with the same
warp, then convert.

- [ ] **Step 3: Drive `t` from the deadline**

Track the current pair's scheduled start time. Render continuously;
`t = (now - start) / frame_ms`, clamped to [0,1]. When `t` reaches 1,
advance the pair and decode the next frame; never decode more than one
ahead. Without audio, use the same wall-clock deadline the presentation
path already uses.

- [ ] **Step 4: Toggle**

`M` switches shader 1 and shader 2, updates the title, and does not
change the frame pair or the clock. Interpolation off must be visually
identical to Task 3's output.

- [ ] **Step 5: Build, run, verify**

Run: `cmake --build build && ctest --test-dir build`
Expected: 100%.

Run the player on a clip with motion; toggle `M`; confirm the
interpolated path shows smoother motion with block-edged warping, never
a skipped or repeated frame, and that seeking resets the pair.

- [ ] **Step 6: Commit**

```bash
git add src/tools/bink_gl_player.c src/tools/bink_gl_shaders.h
git commit -m "tools: interpolate between frames in the GL player"
```

---

### Task 5: Documentation

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Describe the tool**

Add `bink_gl_player` to the tools section: what it needs (SDL2 or SDL3,
OpenGL 3.2), the two shaders, and the `M` key. One short paragraph.

- [ ] **Step 2: Check the build notes**

Run: `grep -n "bink_sdl_player\|BINK_BUILD_TOOLS" README.md`
Expected: the new tool is mentioned wherever the SDL player is.

- [ ] **Step 3: Build and run the suite**

Run: `cmake --build build && ctest --test-dir build`
Expected: 100%.

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "docs: describe the OpenGL player"
```

---

## Self-Review

- **Spec coverage:** motion accessor → Task 1; shared audio and seek →
  Task 2; presentation shader, loader and pacing → Task 3; interpolation,
  pair holding and toggle → Task 4; docs → Task 5.
- **Review Focus:** odd sizes → Task 1 (grid size) and Task 4 (block
  math); no motion → Task 4 (mv 0 crossfade); seek → Task 4 (reset pair);
  pacing → Task 3/4 (one frame ahead); missing symbol → Task 3 (named
  failure).
- **Type consistency:** `bink_get_motion` is declared once and used with
  the same signature in Task 4; `BinkPlayback` names match Task 2's
  interface block throughout.
- **Proportion:** five tasks, signatures and assertions over bodies; the
  long code is the shader text, which the spec fixes.
