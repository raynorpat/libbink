# Development

How to build the variants you need while working on libbink, run and
extend the tests, and check a change before sending it. Basic build and
install commands are in [`../INSTALL.md`](../INSTALL.md).

## Before you begin

- CMake 3.16 or newer and a C99 compiler.
- For the media tests: the five sample files listed under
  [Sample media](#sample-media). They are not in the repository.
- For the players: SDL2 or SDL3, and optionally Vulkan with shaderc.
- For a Windows cross build: mingw-w64 (`x86_64-w64-mingw32-gcc`).
- For regenerating references: VLC 3.0.23 and Python 3, and optionally
  ffmpeg.

## Build variants

Build directories matching `build*/` are ignored by git, so keep one per
variant.

| Purpose | Configure |
| --- | --- |
| Libraries and tests | `cmake -S . -B build` |
| Everything, including tools and players | `cmake -S . -B build -DBINK_BUILD_TOOLS=ON` |
| Force SDL2 or SDL3 | add `-DBINK_SDL_VERSION=2` or `-DBINK_SDL_VERSION=3` |
| Sanitizers | see [Sanitizer build](#sanitizer-build) |
| Windows from macOS or Linux | see [Windows cross build](#windows-cross-build) |

All targets compile as strict C99 (`C_EXTENSIONS OFF`) with
`-Wall -Wextra`, or `/W3` under MSVC. Keep the build free of warnings.

## Tests

Run the suite from a build directory:

```sh
BINK_TEST_MEDIA="$HOME/Downloads" ctest --test-dir build --output-on-failure
```

Run one test by name:

```sh
ctest --test-dir build -R malformed --output-on-failure
```

A full build with tools and SDL registers 17 tests.

| CTest name | Source | Covers | Needs media |
| --- | --- | --- | --- |
| `open` | `test_open.c` | A missing file returns `NULL`; `bink_close(NULL)` is safe | No |
| `container` | `test_container.c` | Header and frame-index parsing | Partly |
| `bits` | `test_bits.c` | Bit reader, bit writer, Huffman trees | No |
| `dsp` | `test_dsp.c` | IDCT, scale, fill, copy, residue add | No |
| `bundles` | `test_bundles.c` | Each bundle reader | No |
| `frames` | `test_frames.c` | Frame iteration and status codes | Partly |
| `decode` | `test_decode.c` | Decoded planes against the VLC reference hashes | Yes |
| `rgb` | `test_rgb.c` | RGB conversion | Yes |
| `seek` | `test_seek.c` | Keyframe seeking | Yes |
| `modes` | `test_modes.c` | Disk mode and memory mode give the same output | Yes |
| `audio` | `test_audio.c` | Audio decoding against reference hashes, truncated payloads, track isolation | Partly |
| `malformed` | `test_malformed.c` | Corrupt and truncated input is rejected without damaging decoder state | Partly |
| `mix` | `test_mix.c` | Resampling, mixing, and the audio clock in `bink_mix.c` | No |
| `encoder` | `test_encoder.c` | Encode and decode round trips, the image and WAV loaders, `--verify` | No |
| `dump` | `run_dump.cmake` | `bink_dump` writes three frames | Yes |
| `bink_build_help` | | `bink_build --help` exits `0` | No |
| `sdl` | `run_sdl.cmake` | `bink_sdl_player` under SDL's dummy drivers: frame count, seek, audio byte count, missing audio driver | Yes |

`encoder`, `dump`, `bink_build_help`, and `sdl` exist only when the
encoder, the tools, or `bink_sdl_player` are built.

### Sample media

Media tests read these files from the directory named by
`BINK_TEST_MEDIA`:

- `ATI-9700-Animusic-Movie-v1.0.bik`
- `Robotica_1080_bink_4100k.bik`
- `logo_lucas.bik`
- `original.bik`
- `valve.bik`

When `BINK_TEST_MEDIA` is unset, those tests skip. `decode`, `rgb`,
`seek`, `modes`, `dump`, and `sdl` report it (exit code 77 or a `SKIP`
line). The tests marked "Partly" run their synthetic cases and pass
without running the media cases, so a green run without media has not
exercised the decoder against real files. Set the variable before
trusting a result.

### Writing a test

Tests are plain C programs with no framework. `tests/test_common.h`
provides:

- `CHECK(cond)`: print the file, line, and expression, and return `1`.
- `fnv1a`, `fnv1a_rows`, `fnv1a_s16le`: the hashes the references use.
  `fnv1a_rows` hashes only the visible width of each row;
  `fnv1a_s16le` hashes samples low byte first on any host.
- `test_media_dir()`: the value of `BINK_TEST_MEDIA`, or `NULL`.

To add a test to an existing file, write a `static int test_<name>(void)`
that returns `0` on success and call it from that file's `main`. To add a
new program, add it to `tests/CMakeLists.txt` with `add_executable`,
`target_link_libraries`, and `add_test`.

For a bug fix, check that the test fails without the fix. When switching
between the two states with `git stash`, touch the changed sources before
rebuilding; a rebuild inside the same second can keep the old objects
and make the comparison meaningless.

### Reference checksums

`tests/reference_checksums.h` holds FNV-1a hashes of the Y, U, and V
planes at frames 0, 1, 2, 5, and 10 of each sample file, captured from
VLC 3.0.23. `tests/reference_audio.h` holds PCM lengths and hashes for
the three files that have audio. Both are generated; do not edit them by
hand.

Regenerate them only when the reference decoder changes, never to make
a failing decode pass:

```sh
BINK_TEST_MEDIA="$HOME/Downloads" tests/capture_reference.sh
BINK_TEST_MEDIA="$HOME/Downloads" tests/capture_audio.sh
```

Run both from the repository root. `VLC` overrides the VLC path
(default `/Applications/VLC.app/Contents/MacOS/VLC`).
`capture_audio.sh` uses ffmpeg when it is installed and VLC otherwise.
The audio lengths follow FFmpeg, which publishes one block more than
VLC's capture.

## Sanitizer build

Use this for any change to the decoder, the encoder, or a renderer:

```sh
cmake -S . -B build-asan -DBINK_BUILD_TOOLS=ON \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cmake --build build-asan
BINK_TEST_MEDIA="$HOME/Downloads" ctest --test-dir build-asan --output-on-failure
```

Any sanitizer report is a bug, including signed overflow and pointer
overflow reports on input that the decoder goes on to reject.

## Checking the decoder against corrupt input

The decoder must reject malformed files without reading or writing out
of bounds. `tests/test_malformed.c` covers known cases. For a wider
check, mutate a real file under the sanitizer build and decode it: flip
bytes in the header and index, in the per-frame audio size words, and in
the frame payloads, and truncate the file at random points. Decode a
few frames of each mutation with video and every audio track enabled,
read the first RGB byte and the last PCM byte so the buffers are
touched, and seek once. The run is clean when the sanitizers report
nothing; `NULL` from an open function and `BINK_ERROR` from a decode are
both correct outcomes.

There is no fuzz harness in the repository. When a mutation finds a
problem, reduce it to a case in `test_malformed.c`.

## Checking a renderer

The GL, Metal, and Vulkan players can read a drawn frame back and
compare it with the decoder's own RGB conversion:

```sh
BINK_PLAYER_DUMP=out.ppm BINK_PLAYER_TEST_FRAMES=1 \
  ./build/bink_sdlgl_player <video.bik>
```

The player prints `dump mean abs diff <mean>, worst <max>`. A correct
presentation shader gives a mean close to zero; small differences come
from the GPU doing the conversion in floating point. Add
`BINK_PLAYER_TEST_SEEK=<frame>` to check a frame with real content, and
`BINK_PLAYER_T=0` or `BINK_PLAYER_T=1` to check that the interpolation
shader reproduces the current or the next frame at the ends of its
range. The variables are listed in
[`tools.md`](tools.md#environment-variables).

These checks need a display and a GPU, so they are not part of CTest.

## Windows cross build

```sh
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
  -DBINK_BUILD_TOOLS=ON -DBINK_BUILD_TESTS=OFF
cmake --build build-win
```

This builds the libraries, `bink_dump`, `bink_build`, and the two
Direct3D players. The SDL players are skipped unless you pass
`-DSDL3_DIR` or `-DSDL2_DIR` for a Windows SDL. The result confirms that
the Windows sources compile; the Direct3D players have no automated
test and need a Windows machine to run.

## Conventions

- **C99, no extensions, no dependencies in the decoder.** `libbink`
  uses only the C library and `libm`.
- **One public header per library.** `bink.h` and `bink_encoder.h` are
  the installed API. Everything else under `src/` is internal, even
  where the shared library exports the symbol.
- **The decoder follows FFmpeg.** Where FFmpeg and other descriptions of
  the format disagree, match FFmpeg, because the reference hashes come
  from it. Keep the retained FFmpeg copyright headers.
- **Decoder parsers are not bent to fit the encoder.** The test hooks
  `bink_video_read_coeffs` and `bink_video_read_residue` mirror the
  decoder's real parsing so the encoder can be checked against it.
- **Format rules live in the decoder.** The encoder calls
  `bink_plane_layout`, `bink_bundle_init_lengths`, and
  `bink_audio_describe` instead of repeating their arithmetic.
- **Update the docs with the code.** A change to a public function, a
  tool option, an environment variable, or an exit code changes
  `usage.md`, `encoder.md`, or `tools.md` in the same commit.

## Before sending a change

1. The normal build has no warnings.
2. `ctest` passes with `BINK_TEST_MEDIA` set.
3. The sanitizer build passes the same suite.
4. A decoder fix has a test that fails without it.
5. A renderer change has been checked with the readback hook on each
   backend you can run, and cross-compiled for the ones you cannot.
