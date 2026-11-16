# GLSL player — design

Status: awaiting approval. New tool; does not change decoder output.

## Intent

A second player binary, `bink_gl_player`, that presents decoded frames
through OpenGL and offers two selectable GLSL paths:

1. **Presentation** — Y, U and V uploaded as single-channel textures and
   converted to RGB in a fragment shader, replacing SDL's software YUV
   conversion.
2. **Motion-compensated interpolation** — the shader reconstructs
   in-between frames from the decoder's own per-block motion vectors,
   warping both neighbours toward the display time and blending them.

The point of 2 is a test: it exercises the motion field the decoder
extracted, on the GPU, where a wrong vector or a wrong block mapping is
visible immediately.

## Goals

- One binary that builds against SDL2 or SDL3, like the existing player.
- OpenGL 3.2 core profile and GLSL 150, the floor macOS offers.
- Toggle between the two shaders at runtime; the window title names the
  active one.
- Pace on the audio clock when a track is present, wall clock otherwise.
- Expose the decoded motion field through one small read-only accessor.
- Reuse `bink_mix` and share the SDL audio wrapper with the existing
  player instead of copying it.

## Non-goals

- Moving entropy decode, IDCT or motion compensation into the shader.
  The GPU only samples decoded planes.
- A quality interpolator. Occlusion, rotation and block edges will show.
- Alpha playback. The player shows the YUV planes only.
- Replacing `bink_sdl_player`, which stays as the lightweight reference.

## Anti-goals

- Showing a frame whose time has not arrived, or drifting against audio.
- Decoding more than one frame ahead to feed the shader.
- Changing any existing decoder output or the existing player's behaviour.

## Constraints

- macOS exposes OpenGL 3.2 and 4.1 core profiles only; GLSL 150 is the
  portable choice and also builds on Mesa.
- SDL2 and SDL3 differ in context creation and event names; all of that
  stays behind the existing `BINK_USE_SDL3` switches, in as few places as
  possible.
- No new dependency: OpenGL ships with the platform. Every GL 3.2 entry
  point is resolved through `SDL_GL_GetProcAddress` at startup on every
  platform, so the same code path works on Apple, Mesa and Windows
  (whose `opengl32.lib` exports only GL 1.1) and nothing is linked
  against `OpenGL::GL`. The target is built wherever an SDL package is
  found, like the existing player.
- `bink_get_*` accessors are read-only and must not alter decode results.

## Approaches considered

Motion source:

- **A (chosen) — the decoder records the luma motion field.** One
  allocation of two `bw*bh` byte arrays per decoder, filled during decode.
  It is the decoder's own vectors, which is the whole point of a test.
- B — the player re-parses the motion bundles. Duplicates decoder logic
  and breaks when the bitstream readers move.
- C — estimate motion on the GPU from frame differences. Different data;
  tests nothing about the decoder.

Interpolation:

- **A (chosen) — block warp of both neighbours, blended by `t`.** Each
  output pixel reads its block's vector, samples frame N displaced by
  `t*mv` and frame N+1 by `(1-t)*mv`, and mixes.
- B — plain crossfade, no warp. Cheap, but it would not test the motion
  field at all.
- C — GPU optical flow. A separate pass and a different problem.

## Motion field

New public accessor in `src/bink.h`, beside the other frame-data reads:

```c
/**
 * Per-8x8-block motion vectors of the current frame's luma plane, in
 * pixels, indexed [by * blocks_w + bx]. Returns 1 when available, 0 when
 * the file has no video. Vectors are (0,0) for blocks that are not
 * motion-compensated.
 */
int bink_get_motion(const bink object, unsigned long *blocks_w,
                    unsigned long *blocks_h, const signed char **mv_x,
                    const signed char **mv_y);
```

`BinkVideo` gains `signed char *mv_x, *mv_y` sized `bw * bh`, allocated
in `bink_video_init` and cleared at the start of every decoded frame.
`decode_plane` records vectors for plane 0 only: `bink_put_pixels` already
reads the X/Y offsets, so it gains two out-parameters and the caller
stores what it used. Chroma blocks use the same field at 16x16 luma
granularity; that approximation is deliberate.

## Shaders

Both vertex shaders are identical: a full-screen quad of two triangles,
position from a `vec2` attribute, `uv` passed through.

**Presentation fragment shader.** Uniforms: `sampler2D u_y, u_u, u_v`,
`mat3 u_yuv_to_rgb`, `vec3 u_offset`. One `texture()` each, the matrix
multiply, `clamp`. The colour constants live in the player, not the
shader, so a future range change is one C constant.

Frames are uploaded as `GL_R8` textures with `GL_UNPACK_ROW_LENGTH` set
to the plane stride, `GL_LINEAR` minification and `GL_CLAMP_TO_EDGE`, so
non-multiple-of-8 widths and edge sampling are handled by the GPU.

**Interpolation fragment shader.** It samples **two frames**, so the
player holds two full YUV triples (six plane textures) plus one motion
texture. The motion field is the newer frame's, copied into the texture
right after that frame is decoded, because `bink_get_motion` describes
the current frame only.

Uniforms beyond shader 1's: `sampler2D u_mv_prev_*` is not needed — the
two triples are `u_y0/u_u0/u_v0` (older) and `u_y1/u_u1/u_v1` (newer),
plus `sampler2D u_mv` (RG8, one texel per 8x8 luma block, value `mv+128`
so it survives an unsigned texture), `vec2 u_plane_size`, `float
u_block_scale`, `float u_t`.

Block lookup must use pixels, not the integer block count, or planes
whose size is not a multiple of 8 shift by half a block:

```
b  = ivec2(floor(uv * u_plane_size / u_block_scale))
mv = (texelFetch(u_mv, b, 0).rg * 255.0 - 128.0) / u_plane_size
y0 = texture(u_y0, uv + u_t * mv)
y1 = texture(u_y1, uv - (1.0 - u_t) * mv)
y  = mix(y0, y1, u_t)
```

`u_block_scale` is 8.0 for luma and 4.0 for chroma, because a chroma
texel covers 2x2 luma pixels and the field is per 8x8 luma block. That
is the derived warp: the newer frame's block came from the older frame
at `p + mv`, so the content sits at `p + (1-s)*mv` at time `s`. Blocks
with no motion have `mv = 0` and crossfade, which is the right
degenerate behaviour. U and V are warped with the same vector and
converted exactly as in shader 1.

## Player structure

- `src/tools/bink_gl_player.c` — window, GL context, GL entry-point
  loader, textures, loop, keys.
- `src/tools/bink_gl_shaders.h` — the two shader source strings.
- `src/tools/bink_playback.{c,h}` — everything both players share: the
  SDL2/3 audio device (open, queue, queued, pause, close) **and** the
  bink-side glue (`publish_audio`, `mix_current`, `append_pcm`,
  `seek_to_frame`), extracted from `bink_sdl_player.c`. Sharing only the
  device would duplicate ~175 lines of mixing and seek logic, and seek is
  where a divergence hides as quiet desync.

Loop: hold frame N and N+1. Render continuously at display rate; compute
`s` from frame N's scheduled start time. That schedule is the same
deadline the existing player paces on (a wall-clock deadline per picture,
with the audio queue only able to veto), not a device play position:
SDL2 exposes queued bytes at ~93 ms granularity, far coarser than a
frame, so a position derived from it would jitter worse than the
deadline does. When `s` reaches 1, advance N to N+1 and decode the next
frame. Shader 1 is used when interpolation is off, shader 2 when on; both
are compiled at startup so the toggle is a uniform and a texture bind,
not a stall.

Keys: Esc/q quit, Space pause, Left/Right seek one second (a seek resets
the pair), `M` toggles interpolation. The title shows which is active.

## Files

- Create: `src/tools/bink_gl_player.c`, `src/tools/bink_gl_shaders.h`,
  `src/tools/bink_audio_out.c`, `src/tools/bink_audio_out.h`.
- Modify: `src/tools/bink_sdl_player.c` (use the shared audio wrapper),
  `CMakeLists.txt` (new target, OpenGL link, deprecation warning guard),
  `src/bink.h`, `src/bink_video.c`, `src/bink_video.h`, `README.md`,
  `docs/encoder.md` is untouched.
- Test: `tests/test_encoder.c` gains the motion-field check.

## Tests

- **Motion field, unit.** Decode a two-frame clip built with a known
  shift, as `test_skip_and_motion` does, and assert `bink_get_motion`
  reports the expected vector for blocks that moved and `(0,0)` on the
  keyframe. This is the part a headless machine can prove.
- **Regression.** The whole existing suite must stay green; the decoder
  change is additive.
- **Manual.** Run both shaders on any clip with motion, toggle `M`,
  pause and seek. Shader 1 must show the same frames in the same order as
  `bink_sdl_player` — same frame, not the same pixels, since SDL2, SDL3
  and our shader each convert YUV differently. Interpolation on should
  show smoother motion with visible block edges, and must not skip or
  repeat frames.

## Risks

- Context creation or a missing GL 3.2 entry point fails on an old
  driver. The player reports which symbol failed and exits non-zero; no
  automated test depends on a window.
- The loader table is untested on Mesa and Windows here. It is one code
  path, so a mistake shows up on the first run on either, not silently.
- macOS marks OpenGL deprecated; the target suppresses that warning only.
- Motion vectors may point outside the frame; the shader clamps sampling
  through `GL_CLAMP_TO_EDGE`.
- Seeking clears the frame pair, or the interpolation would blend across
  the seek.

## What is left to the builder

Shader text, uniform names, the key that toggles, the audio wrapper's
exact surface, and the CMake wiring.
