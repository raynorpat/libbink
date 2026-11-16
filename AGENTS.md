# AGENTS.md

Instructions for coding agents working in this repository. Human
contributors: see [`CONTRIBUTING.md`](CONTRIBUTING.md), which this file
follows.

## Project

C99 Bink 1 decoder (`src/`), encoder library (`src/encoder/`), tools and
players (`src/tools/`), CTest programs (`tests/`). The source map is in
[`docs/architecture.md`](docs/architecture.md).

## Commands

```sh
# configure and build everything
cmake -S . -B build -DBINK_BUILD_TOOLS=ON
cmake --build build

# all tests; the media directory holds the five sample .bik files
BINK_TEST_MEDIA="$HOME/Downloads" ctest --test-dir build --output-on-failure

# one test
ctest --test-dir build -R malformed --output-on-failure

# sanitizer build
cmake -S . -B build-asan -DBINK_BUILD_TOOLS=ON \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cmake --build build-asan
BINK_TEST_MEDIA="$HOME/Downloads" ctest --test-dir build-asan --output-on-failure

# Windows sources compile (needs mingw-w64)
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
  -DBINK_BUILD_TOOLS=ON -DBINK_BUILD_TESTS=OFF
cmake --build build-win

# renderer output against the decoder's RGB (needs a display)
BINK_PLAYER_DUMP=out.ppm BINK_PLAYER_TEST_FRAMES=1 \
  ./build/bink_sdlgl_player <video.bik>
```

Use a `build*/` directory for every build; they are ignored by git.

## Rules

Each rule names the failure it prevents.

- **Set `BINK_TEST_MEDIA` before reporting that tests pass.** Without
  it, `container`, `frames`, `audio`, and `malformed` return success
  after running only their synthetic cases, so a green run has not
  decoded a real file. If the sample files are not available, say so in
  the report.
- **After changing `BinkRendererBackend` or `bink_d3d_renderer.h`, run
  the interpolation shader through the readback hook and cross-compile
  the Direct3D players.** In `7e2e0c8` an interface change dropped the
  motion block counts; every block read vector (0,0) and interpolation
  became a plain cross-fade with no error. The same omission survived in
  the Direct3D players until the audit, because they are never run on
  macOS or Linux.
- **When comparing a test with and without a fix, touch the changed
  sources before each rebuild.** A `git stash` followed by a rebuild in
  the same second left stale objects during the audit, and the test
  appeared to pass without its fix.
- **A regression test for the decoder must reach the code it guards.**
  The first version of the truncated-media test in
  `tests/test_malformed.c` was rejected at open by the file-size field
  and never ran the video decoder. Confirm the test fails without the
  fix.
- **Report what was not run.** The Direct3D players cannot run on a
  non-Windows host and the GPU players need a display. Say
  "compile-checked only" for those instead of "verified".

## Boundaries

Always:

- Keep the build free of warnings under `-Wall -Wextra`.
- Update `docs/usage.md`, `docs/encoder.md`, or `docs/tools.md` in the
  same change as a public function, tool option, environment variable,
  or exit code.

Ask first:

- Changing `src/bink.h` or `src/encoder/bink_encoder.h`.
- Regenerating `tests/reference_checksums.h` or
  `tests/reference_audio.h`.
- Adding a dependency, or changing anything under `third_party/`.
- Making the decoder stricter than FFmpeg about what it accepts; real
  files may depend on the looser behaviour.

Never:

- Edit the generated reference headers by hand.
- Remove the FFmpeg copyright headers from the files derived from it.
- Edit the dated records under `docs/superpowers/`; they describe past
  designs and are not kept current.

## Commits

Subject `area: what changed` (`fix:`, `feat:`, `docs:`, `encoder:`,
`decoder:`, `tools:`, `test:`, `build:`). The body says why and what was run to verify it.
Commit only when asked.
