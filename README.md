# libbink

`libbink` is a small C99 library for decoding Bink 1 video files (`.bik`)
produced by RAD Game Tools. It offers an opaque handle and a simple
open/info/decode/seek API in the spirit of
[libsmacker](https://github.com/kylefarris/libsmacker).

## Status

Video and audio decoding are implemented for Bink 1 files with the `BIKf`,
`BIKg`, `BIKh`, and `BIKi` signatures. A track is decoded when it is
enabled. Both Bink Audio DCT (`flags & 0x1000`) and RDFT are supported.
The SDL player mixes every enabled track. A payload the decoder cannot
finish drops that track for the frame; the picture and the other tracks
continue. Bink 2 (`KB2*`) and the legacy `BIKb` plane decoder are not
supported in this version.

Decoded frames are available as padded YUV 4:2:0 planes (plus alpha when
present) and as a lazily converted packed RGB24 buffer. Keyframe seeking
walks back to the nearest frame flagged as a keyframe in the container
index.

## License

`libbink` is licensed under the GNU Lesser General Public License,
version 2.1 or later. See `LICENSE`.

The Bink 1 decoder is a port of FFmpeg's `libavcodec/bink.c`,
`libavcodec/binkdsp.c`, `libavcodec/binkdata.h`, and
`libavcodec/binkaudio.c`. Those files are copyright the FFmpeg developers
and licensed under the LGPL-2.1-or-later.
See `docs/bink.md` for implementation notes and attribution.

## Build

```sh
cmake -S . -B build -DBINK_BUILD_TOOLS=ON
cmake --build build
```

Requires CMake 3.16 or newer and a C99 compiler. The SDL player is optional
and built only when an SDL3 or SDL2 CMake package is available.

Options:

- `BINK_BUILD_STATIC` (default `ON`)
- `BINK_BUILD_SHARED` (default `ON`)
- `BINK_BUILD_TOOLS` (default `OFF`)
- `BINK_BUILD_TESTS` (default `ON`)
- `BINK_SDL_VERSION` (`AUTO`, `2`, or `3`; default `AUTO`)

`BINK_SDL_VERSION=AUTO` prefers SDL3 and falls back to SDL2. Force a backend
with `-DBINK_SDL_VERSION=3` or `-DBINK_SDL_VERSION=2`.

See `INSTALL.md` for platform notes and install commands.

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

- `bink_sdl_player video.bik` - SDL3 (preferred) or SDL2 playback.
  Enables every audio track and mixes them. Esc/Q quits, Space pauses,
  Left/Right seek one second.
- `bink_dump [--frames N] video.bik [outdir]` - headless PPM frame dump and
  checksums. Writes `frame_%05u.ppm` (P6) files.

## Tests

The test suite includes synthetic unit tests that need no media, plus
reference and end-to-end tests that read the five sample files from the
directory named by `BINK_TEST_MEDIA`:

```sh
BINK_TEST_MEDIA="$HOME/Downloads" ctest --test-dir build --output-on-failure
```

Reference checksums in `tests/reference_checksums.h` were captured from VLC
3.0.23 decoded YUV4MPEG2 output. `tests/capture_reference.sh` and
`tests/gen_reference.py` regenerate them. Tests that need sample media skip
when `BINK_TEST_MEDIA` is unset.
