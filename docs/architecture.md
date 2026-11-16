# Architecture

This page maps the source tree: which file owns what, how a frame moves
through the decoder and the encoder, and how the players are layered. For
the file format itself see [`bink.md`](bink.md); for the public API see
[`usage.md`](usage.md) and [`encoder.md`](encoder.md).

## Build targets

| Target | Kind | Sources | Depends on |
| --- | --- | --- | --- |
| `bink_static`, `bink_shared` (`libbink`) | Library | `src/*.c` | C library, `libm` |
| `bink_encoder` (`libbinkenc`) | Static library | `src/encoder/*.c` | `libbink`, vendored `stb_image.h` |
| `bink_dump` | Tool | `src/tools/bink_dump.c` | `libbink` |
| `bink_build` | Tool | `src/tools/bink_build.c` | `libbinkenc` |
| `bink_sdl_player` | Player | `bink_sdl_player.c` + playback | `libbink`, SDL2 or SDL3 |
| `bink_sdlgl_player`, `bink_sdlmetal_player`, `bink_sdlvulkan_player` | Players | player core + one renderer + playback | `libbink`, SDL, a GPU API |
| `bink_d3d9_player`, `bink_d3d11_player` | Players | `bink_win32_player.c` + one renderer + playback | `libbink`, Direct3D, `winmm` |

The decoder has no dependency on the encoder or the tools. The encoder
links the decoder and reuses its internal headers, so the bit order,
Huffman tables, plane geometry, and audio block parameters are defined
once.

## Decoder (`src/`)

| File | Owns |
| --- | --- |
| `bink.h` | The public API. The only installed decoder header |
| `bink.c` | Opening, header and frame-index parsing, packet reads, the public functions |
| `bink_internal.h` | `struct bink_t` and the limits (7 tracks, 7680x4800, 1,000,000 frames) |
| `bink_bits.c/.h` | LSB-first bit reader, bit writer, Huffman tree reading and table selection |
| `bink_huffdata.h` | The sixteen built-in Huffman tables |
| `bink_video.c/.h` | Bundles, block decoding, motion vectors, plane buffers, YUV to RGB |
| `binkdata.h` | Scan order, quantizer matrices, and pattern tables from FFmpeg |
| `bink_dsp.c/.h` | Integer IDCT, block scale, fill, copy, and residue add |
| `bink_audio.c/.h` | Bink Audio DCT and RDFT decoding, plus the forward DCT and block writer the encoder uses |

### How a frame is decoded

1. **Open.** `bink_open_*` parses the header and the frame index into
   offsets, sizes, and keyframe flags. `BINK_MODE_MEMORY` holds the whole
   file in memory; `BINK_MODE_DISK` keeps the `FILE *` and reads one
   packet at a time.
2. **Read the packet.** `bink_first`, `bink_next`, and
   `bink_seek_keyframe` copy the frame's packet into one reusable buffer
   and split it into one payload per audio track followed by the video
   payload.
3. **Decode video.** `bink_video_decode` swaps the current and reference
   plane sets, then decodes the planes into the current set. Each plane
   reads its nine bundle tree headers, then for every row of 8x8 blocks
   refills the bundles and applies one block operation per block.
4. **Decode audio.** Each enabled track decodes its payload into a PCM
   buffer that grows on demand up to the size the header declares for the
   track.
5. **Publish.** `bink_get_plane` returns the current planes.
   `bink_get_video` converts them to RGB24 on first use and caches the
   result until the next decode. `bink_get_motion` returns the luma
   motion vectors recorded while decoding.

### Properties the decoder keeps

- **Bounds are checked, not trusted.** Every bit read reports
  exhaustion, every bundle write is checked against its buffer, and a
  motion reference outside the reference plane leaves the block
  unchanged.
- **A failed video decode does not corrupt state.** The plane swap is
  undone, so the planes and the RGB buffer still show the last good
  frame after `BINK_ERROR`.
- **A failed audio payload is local.** That track publishes nothing for
  the frame; the picture and the other tracks continue.
- **Host endianness does not matter.** Container integers are read byte
  by byte; PCM is published in host order.

`tests/test_malformed.c` pins the first two properties.

### Internal functions shared with the encoder

Some functions in `bink_video.h`, `bink_bits.h`, and `bink_audio.h` exist
so the encoder and the tests can reach the decoder's own tables and
parsers: `bink_plane_layout`, `bink_bundle_init_lengths`,
`bink_video_scan`, `bink_video_intra_quant`, `bink_video_inter_quant`,
`bink_video_read_coeffs`, `bink_video_read_residue`, `bink_huff_best`,
`bink_audio_describe`, and `bink_audio_write_dct_block`. They are not
installed and are not a stable API. The shared library exports them only
because the tests link against it.

## Encoder (`src/encoder/`)

| File | Owns |
| --- | --- |
| `bink_encoder.h` | The public API. The only installed encoder header |
| `bink_encoder.c` | `bink_enc_encode`: validation, header, frame loop, index patch, verify call |
| `bink_enc_video.c/.h` | RGB to YUV 4:2:0, per-block mode decision, motion search, bundle writing |
| `bink_enc_dct.c/.h` | Forward 8x8 DCT, quantization, coefficient-tree writer |
| `bink_enc_audio.c/.h` | Splits the PCM stream into Bink audio blocks per video frame |
| `bink_enc_verify.c/.h` | Decodes the output with `libbink` and compares it with the source |
| `bink_enc_image.c/.h` | PPM loader, and BMP/PNG/JPEG through stb_image |
| `bink_enc_stb.c` | The single translation unit that compiles `stb_image.h` |
| `bink_enc_wav.c/.h` | PCM WAV loader (8, 16, or 24-bit) |

### How a file is encoded

1. `bink_enc_encode` validates the source, opens one audio encoder per
   track, and writes the header and a frame index filled with zeros.
2. For each frame it calls `read_frame`, converts RGB to planes, and
   writes the video payload with `bink_enc_write_frame`. The encoder
   keeps its own reconstruction of each plane (`BinkEncPlanes`), which is
   what the decoder will see, and predicts the next frame from that.
3. For each track it calls `bink_enc_audio_frame`, which pulls samples
   through `read_audio` and writes that frame's audio payload.
4. It writes the packet (audio payloads, then video) and records its
   offset.
5. After the last frame it seeks back and fills in the file size, the
   largest packet size, and the frame index.
6. With `verify` set, `bink_enc_verify` reopens the file with the
   decoder and checks every frame's visible planes and every track's
   decoded samples against the source. The tolerances are listed in
   [`encoder.md`](encoder.md).

The image and WAV loaders are used by `bink_build` and by the tests. A
program that links `libbinkenc` supplies its own `read_frame` and
`read_audio` callbacks.

## Tools and players (`src/tools/`)

`bink_dump.c` and `bink_build.c` are single-file programs over the two
public APIs. The players share three layers:

```text
                 bink_sdl_player_core.c          bink_win32_player.c
                 (SDL window, keys, timing)      (Win32 window, keys, timing)
                        |                               |
        +---------------+---------------+       +-------+-------+
        |               |               |       |               |
  bink_gl_renderer  bink_metal_   bink_vulkan_  bink_d3d9_   bink_d3d11_
        .c          renderer.m    renderer.c    renderer.c   renderer.c
        (bink_renderer.h interface)             (bink_d3d_renderer.h)

                 bink_playback.c  (mix, queue, seek; shared by all)
                        |
          bink_playback_sdl.c  or  bink_playback_win32.c   (audio device)
                        |
                 bink_mix.c  (resample, sum, timing arithmetic)
```

| File | Owns |
| --- | --- |
| `bink_mix.c/.h` | Pure arithmetic: linear resampling with carried phase, saturating mix, audio-clock helpers. No device or SDL code, so `tests/test_mix.c` covers it directly |
| `bink_playback.c/.h` | Mixes the current frame's tracks and queues them; rebuilds the queue after a seek |
| `bink_playback_sdl.c`, `bink_playback_win32.c` | The audio device: SDL audio or `waveOut` |
| `bink_renderer.h` | `BinkRendererBackend`, the function table an SDL renderer fills in |
| `bink_sdl_player_core.c/.h` | `bink_player_run`: the loop shared by the GL, Metal, and Vulkan players |
| `bink_sdlgl_player.c`, `bink_sdlmetal_player.c`, `bink_sdlvulkan_player.c` | A backend table and a call to `bink_player_run` |
| `bink_gl_shaders.h` | The GLSL presentation and interpolation shaders |
| `bink_d3d_renderer.h` | The interface the two Direct3D renderers implement |
| `bink_sdl_player.c` | The original player. It uses SDL's 2D renderer with a YUV texture and does not use the core or a GPU backend |

### The frame pair

The GPU players keep two decoded frames on the GPU, in slots 0 and 1:
the frame on screen and the one after it. `slot_cur` names the current
one and flips on every advance. The interpolation shader reads both
slots and the newer frame's motion field, uploaded as one two-byte texel
per 8x8 luma block (`vector + 128`). `t` runs from 0 to 1 across the
frame interval. In presentation mode only the current slot is drawn.

### Adding a renderer

To add an SDL-based renderer:

1. Implement the seven functions in `BinkRendererBackend`
   (`src/tools/bink_renderer.h`). `readback` must return the last drawn
   frame as top-down RGB24 at the video size.
2. Add a `main` that fills in the table and calls `bink_player_run`, as
   `bink_sdlgl_player.c` does.
3. Add the executable to `CMakeLists.txt` with the core, the mix, and
   the playback sources.
4. Check it with the readback hook described in
   [`development.md`](development.md#checking-a-renderer).

## Tests (`tests/`)

Each `test_*.c` is a standalone program registered with CTest. The list
and what each one covers is in [`development.md`](development.md#tests).
