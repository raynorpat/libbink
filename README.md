# libbink

`libbink` is a small C99 library for decoding Bink 1 video files (`.bik`)
produced by RAD Game Tools. It offers an opaque handle and a simple
open/info/decode/seek API in the spirit of
[libsmacker](https://github.com/kylefarris/libsmacker).

## Status

Video decoding is implemented for Bink 1 files with the `BIKf`, `BIKg`,
`BIKh`, and `BIKi` signatures. Bink Audio, Bink 2 (`KB2*`), and the legacy
`BIKb` plane decoder are not supported in this version.

Decoded frames are available as padded YUV 4:2:0 planes (plus alpha when
present) and as a lazily converted packed RGB24 buffer.

## License

`libbink` is licensed under the GNU Lesser General Public License,
version 2.1 or later. See `LICENSE`.

The Bink 1 decoder is a port of FFmpeg's `libavcodec/bink.c`,
`libavcodec/binkdsp.c`, and `libavcodec/binkdata.h`. Those files are
copyright the FFmpeg developers and licensed under the LGPL-2.1-or-later.
See `docs/bink.md` for implementation notes and attribution.

## Build

```sh
cmake -S . -B build -DBINK_BUILD_TOOLS=ON
cmake --build build
```

Options:

- `BINK_BUILD_STATIC` (default `ON`)
- `BINK_BUILD_SHARED` (default `ON`)
- `BINK_BUILD_TOOLS` (default `OFF`)
- `BINK_BUILD_TESTS` (default `ON`)
- `BINK_SDL_VERSION` (`AUTO`, `2`, or `3`)

See `INSTALL.md` for details.

## Usage

```c
#include <bink.h>

bink b = bink_open_file("video.bik", BINK_MODE_DISK);
if (!b)
    return 1;

bink_enable_video(b, 1);
bink_first(b);
/* bink_get_plane(b, BINK_PLANE_Y), bink_get_video(b), ... */
bink_close(b);
```

Full documentation is in `docs/usage.md`.

## Tools

- `bink_sdl_player video.bik` - SDL2/SDL3 playback.
- `bink_dump video.bik [outdir]` - headless PPM frame dump and checksums.

## Tests

Tests use the sample media named by `BINK_TEST_MEDIA` (defaults are read from
`$HOME/Downloads` when set by the test harness):

```sh
BINK_TEST_MEDIA="$HOME/Downloads" ctest --test-dir build --output-on-failure
```
