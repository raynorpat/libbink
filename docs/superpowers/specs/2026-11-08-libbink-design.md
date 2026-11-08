# libbink design

Date: 2026-11-08
Status: approved

## Intent

Build `libbink`, a small cross-platform C library that decodes RAD Game Tools
Bink 1 video files (`.bik`). It should feel like `libsmacker`: an opaque handle,
simple open/info/decode/seek calls, library-owned frame buffers, a static and
shared build, and an SDL playback example that works with SDL2 or SDL3.

The immediate target is the five Bink files in the user's Downloads folder.
All five are Bink 1, version `BIKi`:

| File | Coded size | Frames | Audio |
| --- | --- | ---: | --- |
| `valve.bik` | 1920x1080, 25 fps | 291 | Bink Audio DCT |
| `Robotica_1080_bink_4100k.bik` | 1440x1080, ~24 fps | 498 | none |
| `ATI-9700-Animusic-Movie-v1.0.bik` | 640x480, ~30 fps | 6120 | Bink Audio DCT |
| `logo_lucas.bik` | 640x480, 30 fps | 266 | Bink Audio DCT |
| `original.bik` | 512x384, 25 fps | 280 | Bink Audio RDFT |

Every test file marks only frame 0 as a keyframe in its frame index, so seeking
may require decoding from the start.

## Goals

- Decode Bink 1 video for `BIKf`, `BIKg`, `BIKh`, and `BIKi` files.
- Output native YUV 4:2:0 planes (plus an alpha plane when present), and a lazy
  packed RGB24 buffer for the simple single-buffer API.
- Parse Bink container metadata: dimensions, frame count, frame rate, flags,
  audio track descriptors, and the frame index/keyframe table.
- Support disk-backed and memory-backed opening, like libsmacker.
- Provide `bink_first`, `bink_next`, and `bink_seek_keyframe`.
- Ship `bink_sdl_player` (SDL3 preferred, SDL2 fallback) and `bink_dump`
  (headless PPM dump plus checksums).
- Build warning-clean as C99 with no third-party runtime dependencies.
- Remain robust enough to reject malformed/truncated files without crashing.

## Non-goals

- Bink Audio decoding in v1. Audio metadata is parsed and reported; audio
  packets are skipped. Phase 2 adds DCT and RDFT audio.
- Bink 2 (`KB2*`) files.
- The old `BIKb` plane decoder.
- Hardware-accelerated video decode.
- Exact ABI compatibility with RAD's `bink.h`.

## Anti-goals

- A thin wrapper that requires FFmpeg at runtime.
- Unbounded allocations or out-of-bounds reads on untrusted input.
- A player that only works with one SDL major version.
- Silently producing garbage frames when the bitstream is corrupt.

## Constraints

- The MultimediaWiki Bink page is incomplete: its DCT-coefficient, residue, and
  quantizer sections are marked TODO. A from-scratch decoder based only on that
  page cannot decode real files.
- The practical foundation is FFmpeg's LGPL-2.1-or-later Bink 1 decoder
  (`libavcodec/bink.c`, `binkdsp.c`, `binkdata.h`).
- The library will therefore be LGPL-2.1-or-later, matching libsmacker.
  Ported files retain FFmpeg copyright headers, and the README/docs credit
  FFmpeg.
- C99, standard library only. CMake 3.16 or newer for the supplied build.

## Approaches considered

1. **Port FFmpeg's Bink 1 decoder into a standalone library. (chosen)**
   Reliable, decodes all test media, no runtime dependency. Cost: LGPL
   obligations and a careful, attributed port rather than original code.
2. **Write from scratch from the MultimediaWiki page.**
   Attractive for originality, but the required DCT/residue/quantizer details
   are absent. High risk of never decoding real files. Rejected.
3. **Wrap libavcodec.**
   Least decoder code, but adds a large runtime dependency and conflicts with
   the libsmacker-style small-library goal. Rejected.

The user chose option 1 and chose video-first, with audio in phase 2.

## Public API

```c
typedef struct bink_t *bink;

#define BINK_DONE   0x00
#define BINK_MORE   0x01
#define BINK_LAST   0x02
#define BINK_ERROR  -1

#define BINK_MODE_DISK    0x00
#define BINK_MODE_MEMORY  0x01

#define BINK_PLANE_Y 0
#define BINK_PLANE_U 1
#define BINK_PLANE_V 2
#define BINK_PLANE_A 3

#define BINK_FLAG_ALPHA 0x00100000
#define BINK_FLAG_GRAY  0x00020000

bink bink_open_file(const char *filename, unsigned char mode);
bink bink_open_filepointer(FILE *file, unsigned char mode);
bink bink_open_memory(const unsigned char *buffer, unsigned long size);
void bink_close(bink object);

char bink_info_all(const bink object, unsigned long *frame,
                   unsigned long *frame_count, double *usf);
char bink_info_video(const bink object, unsigned long *w, unsigned long *h,
                     unsigned long *flags);
char bink_info_audio(const bink object, unsigned char *track_mask,
                     unsigned char channels[7], unsigned char bitdepth[7],
                     unsigned long audio_rate[7]);

char bink_enable_video(bink object, unsigned char enable);
char bink_enable_audio(bink object, unsigned char track,
                       unsigned char enable); /* stub in v1 */

char bink_first(bink object);
char bink_next(bink object);
char bink_seek_keyframe(bink object, unsigned long frame);

const unsigned char *bink_get_video(const bink object);      /* RGB24, lazy */
const unsigned char *bink_get_plane(const bink object, int plane);
int bink_get_stride(const bink object, int plane);
const unsigned char *bink_get_audio(const bink object, unsigned char track);
unsigned long bink_get_audio_size(const bink object, unsigned char track);
```

Behaviour:

- Decoding is disabled until `bink_enable_video(object, 1)`.
- `bink_first` rewinds and decodes frame 0. `bink_next` advances one frame.
- Return codes follow libsmacker: `BINK_MORE`, `BINK_LAST`, `BINK_DONE`,
  `BINK_ERROR`.
- `bink_get_video` returns `w * h * 3` bytes of packed RGB24, converted lazily
  from the planes only when called. This is the libsmacker-style simple path.
- `bink_get_plane` returns Y/U/V/A planes. Y is `w x h`; U and V are
  `ceil(w/2) x ceil(h/2)`; A is `w x h`. Row order is top to bottom.
- Each plane has its own stride from `bink_get_stride`.
- Plane pointers are library-owned and valid until `bink_close`; contents are
  replaced on each decoded frame.
- Audio metadata is reported, but `bink_get_audio` returns NULL and
  `bink_get_audio_size` returns 0 in v1.

## Decoder design

### Container and frame index

- Parse the 4-byte signature and accept `BIKf`, `BIKg`, `BIKh`, `BIKi`.
  Reject `BIKb`, `BIKk`, and `KB2*` with a clear diagnostic.
- Read file size, frame count, largest frame size, dimensions, fps numerator
  and denominator, flags, audio track count, audio descriptors, and the frame
  offset table.
- Parse the frame table up front. Each entry stores the byte offset, size, and
  whether it is a keyframe (the low bit of the next offset, matching Bink's
  encoding).
- Validate monotonic offsets and cap dimensions/frame counts.
- Disk mode seeks per frame. Memory mode copies the file once and slices it.

### Frame packet layout

- For each audio track, read a little-endian 32-bit chunk size and skip that
  payload. Reject a chunk that runs past the frame.
- The remaining bytes are the video packet.
- For `BIKi`, skip a 32-bit luma plane size prefix, then a 32-bit alpha plane
  size prefix when alpha is present. The sizes are not needed for sequential
  decoding.
- Decode planes Y, U, V sequentially. For version `h` and later, the U and V
  destinations are swapped as FFmpeg does.
- Each plane ends on a 32-bit boundary.

### Bitstream and bundles

- LSB-first bit reader with a 32-bit cache and explicit bounds checks.
- Huffman trees: build a small lookup table from FFmpeg's
  `bink_tree_bits`/`bink_tree_lens` tables. The tables are read LSB-first.
- Port the bundle decoders: 4-bit run, 4-bit signed motion, 4-bit RLE block
  types, 4-bit pair patterns, context-coded 8-bit colours, and 16-bit DC deltas.
- Port the block scan order, pattern tables, and intra/inter quantizer tables.

### Block types

Implement all modern Bink 1 block types:

- skip
- motion copy
- run fill
- motion + residue
- intra DCT
- fill
- inter DCT
- pattern fill
- raw
- scaled 16x16 (with sub-block type)

The old `BIKb` decoder is not included.

### DSP

- Integer IDCT (`idct_put`, `idct_add`) with the same rounding as FFmpeg.
- 8x8 to 16x16 block scaling.
- Overlap-safe 8x8 motion copy.
- 8x8/16x16 byte fill.
- Signed residue add with byte wrap, matching FFmpeg.

### Frame buffers

- Allocate block-aligned, padded planes so the decoder can write whole 8x8
  chroma blocks at 1080p (540 visible chroma rows, 544 decoded rows).
- Keep the previous decoded frame for motion compensation.
- Zero or reset the previous frame on seek so a keyframe cannot copy stale
  pixels from an unrelated position.

### Colour conversion

- `bink_get_video` converts YUV to RGB24 using BT.601 limited-range coefficients
  for Bink 1 (FFmpeg treats Bink 1 as MPEG range). Alpha is ignored in v1.
- The SDL player uses `SDL_UpdateYUVTexture` directly for speed; `bink_dump`
  uses the RGB path so its PPMs are directly comparable with other players.

### Seeking

- `bink_seek_keyframe` walks back to the nearest keyframe at or before the
  requested frame, resets state, and decodes that keyframe.
- Callers repeat `bink_next` to reach the exact frame.
- Because the test files only mark frame 0, seeking far into them is linear.
  This is documented.

## Tools

- `bink_sdl_player file.bik`: opens the file, prints metadata, creates a
  resizable window, displays YUV frames, and supports Esc/Q quit, Space pause,
  and Left/Right seek. SDL3 is preferred; SDL2 is a compile-time fallback via
  `BINK_SDL_VERSION`.
- `bink_dump file.bik [outdir]`: decodes every frame, writes `frame_%05u.ppm`,
  and prints a per-frame checksum. Used for headless verification and visual
  spot checks.

## Build and packaging

- CMake project `libbink`, version 0.1.0, C99.
- Options: `BINK_BUILD_STATIC` (ON), `BINK_BUILD_SHARED` (ON),
  `BINK_BUILD_TOOLS` (OFF), `BINK_SDL_VERSION` (`AUTO`, `2`, or `3`).
- Targets: `bink_static`, `bink_shared`, alias `Bink::bink`.
- Install the headers and libraries with GNUInstallDirs.
- Warning flags `-Wall -Wextra`, or `/W3` on MSVC.
- Shared library version and SOVERSION set for future compatibility.

## Documentation

- `README.md`: introduction, feature status, license/attribution, build and
  usage summary.
- `INSTALL.md`: build options, SDL selection, installation.
- `docs/usage.md`: full API reference, ownership, formats, seeking, return
  codes, and limitations.
- `docs/bink.md`: container, bitstream, Huffman trees, bundles, block types,
  DCT/residue, version differences, and differences between the wiki prose and
  the implementation.
- `LICENSE`: LGPL-2.1-or-later text.

## Verification

- Build warning-clean with Apple clang.
- Decode all five test files headlessly; confirm frame counts and stable
  checksums.
- Spot-check frame 0, a middle frame, and the last frame against VLC.
- Run 1080p files under ASan and UBSan.
- Build and run the SDL player with SDL3 and with SDL2.
- Test a truncated or malformed file for graceful failure.
- Check disk mode and memory mode produce identical checksums.
- Optionally install FFmpeg via Homebrew for pixel-level reference comparisons.

## Phase 2: Bink Audio

- Add `bink_audio.c` with Bink Audio DCT (four test files) and RDFT
  (`original.bik`).
- Decode per-frame audio packets into interleaved signed 16-bit PCM.
- Implement or vendor the required inverse transforms without adding a runtime
  dependency.
- Wire audio into `bink_sdl_player` and document the new API behaviour.
- The v1 API already reserves `bink_enable_audio`, `bink_get_audio`, and
  `bink_get_audio_size`.

## Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| LSB-first Huffman bit ordering is easy to get backwards | Validate frame 0 immediately; compare output with VLC |
| IDCT rounding differs from the reference | Port the FFmpeg macros exactly; use visual/PSNR comparison |
| 1080p chroma padding overruns | Block-aligned planes plus ASan/UBSan runs |
| Corrupt input causes crashes | Bounds-check every bitstream and frame read |
| LGPL compliance is missed | Retain FFmpeg headers and add attribution in README/LICENSE |
| Seeking is slow because of the single keyframe | Document it; optionally add a decode-forward helper later |

## Decisions left to the builder

- Exact internal file split between `bink.c`, `bink_video.c`, and `bink_dsp.c`.
- Lookup-table layout for Huffman decoding.
- Whether the player defaults to YUV textures or RGB; YUV is expected for speed.
- Whether to add a `bink_seek` convenience that decodes forward to an exact
  frame.
