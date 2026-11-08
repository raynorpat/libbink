# Installing libbink

## Requirements

- CMake 3.16 or newer
- A C99 compiler (tested with Apple Clang on macOS)
- Optional: SDL3 or SDL2 development packages for `bink_sdl_player`

The library itself has no external dependencies beyond the C standard
library.

## Build options

| Option | Default | Meaning |
| --- | --- | --- |
| `BINK_BUILD_STATIC` | `ON` | Build `bink_static` |
| `BINK_BUILD_SHARED` | `ON` | Build `bink_shared` (versioned SONAME on Unix) |
| `BINK_BUILD_TOOLS` | `OFF` | Build `bink_dump` and `bink_sdl_player` |
| `BINK_BUILD_TESTS` | `ON` | Build and register the CTest suite |
| `BINK_SDL_VERSION` | `AUTO` | `AUTO`, `2`, or `3` |

At least one of `BINK_BUILD_STATIC` and `BINK_BUILD_SHARED` should be
enabled. `Bink::bink` aliases the shared library when it is built, otherwise
the static library; the tools link whichever target is active.

## Basic build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Tools and SDL selection

```sh
cmake -S . -B build -DBINK_BUILD_TOOLS=ON
cmake --build build
```

With `AUTO`, CMake looks for SDL3 first and then SDL2. If neither is found,
`bink_sdl_player` is skipped with a warning and `bink_dump` is still built.
Force a backend explicitly:

```sh
cmake -S . -B build-sdl3 -DBINK_BUILD_TOOLS=ON -DBINK_SDL_VERSION=3
cmake -S . -B build-sdl2 -DBINK_BUILD_TOOLS=ON -DBINK_SDL_VERSION=2
```

On Homebrew macOS installations, SDL CMake packages are normally found
without extra flags. If CMake cannot find them, point it at the prefix:

```sh
cmake -S . -B build -DBINK_BUILD_TOOLS=ON \
  -DCMAKE_PREFIX_PATH="$(brew --prefix sdl3)"
```

Use `brew --prefix sdl2` for the SDL2 backend instead.

## Install

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build
cmake --install build
```

This installs the libraries, the public header `bink.h`, and the tools that
were built.

## Tests and sample media

Synthetic tests run without any media. Reference and end-to-end tests read
five `.bik` files from the directory named by `BINK_TEST_MEDIA`:

```sh
BINK_TEST_MEDIA="$HOME/Downloads" ctest --test-dir build --output-on-failure
```

If `BINK_TEST_MEDIA` is unset, the media-dependent tests print a skip notice
(and the dump/player wrapper tests return success without running). The
expected files are:

- `ATI-9700-Animusic-Movie-v1.0.bik`
- `Robotica_1080_bink_4100k.bik`
- `logo_lucas.bik`
- `original.bik`
- `valve.bik`

To regenerate the reference checksums from VLC 3.0.23:

```sh
BINK_TEST_MEDIA="$HOME/Downloads" tests/capture_reference.sh
```

This requires VLC at `/Applications/VLC.app/Contents/MacOS/VLC` (override
with the `VLC` environment variable) and Python 3.

## Sanitizer build

```sh
cmake -S . -B build-asan -DBINK_BUILD_TESTS=ON \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cmake --build build-asan
BINK_TEST_MEDIA="$HOME/Downloads" ctest --test-dir build-asan -R decode --output-on-failure
```
