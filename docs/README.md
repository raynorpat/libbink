# libbink documentation

libbink is a C99 library that decodes Bink 1 video (`.bik`), a companion
encoder library that writes `BIKi` files, and a set of command-line tools
and example players built on both. This page routes you to the right
document.

## Start here

| You want to | Read |
| --- | --- |
| Build and install the libraries and tools | [`../INSTALL.md`](../INSTALL.md) |
| Decode a file from your own C program | [`usage.md`](usage.md) |
| Encode images and WAV audio into a `.bik` | [`encoder.md`](encoder.md) |
| Run `bink_dump`, `bink_build`, or one of the players | [`tools.md`](tools.md) |
| Find your way around the source tree | [`architecture.md`](architecture.md) |
| Run the tests, a sanitizer build, or a Windows cross build | [`development.md`](development.md) |
| Understand the container and bitstream | [`bink.md`](bink.md) |
| Send a change | [`../CONTRIBUTING.md`](../CONTRIBUTING.md) |

## Five-minute check

Build everything, decode three frames of a file, and encode them back:

```sh
cmake -S . -B build -DBINK_BUILD_TOOLS=ON
cmake --build build
./build/bink_dump --frames 3 <video.bik> frames
./build/bink_build frames -o roundtrip.bik --verify
```

`bink_dump` prints one line of plane checksums per frame and writes
`frames/frame_00000.ppm` onward. `bink_build --verify` exits `0` when the
encoded file decodes back to the source within the encoder's tolerance.

## What is in the repository

| Path | Contents |
| --- | --- |
| `src/` | The decoder library: `bink.h` is its only public header |
| `src/encoder/` | The encoder library: `bink_encoder.h` is its only public header |
| `src/tools/` | `bink_dump`, `bink_build`, and the players |
| `tests/` | CTest programs, reference checksums, and the scripts that regenerate them |
| `cmake/` | The mingw-w64 toolchain file for Windows cross builds |
| `third_party/stb/` | `stb_image.h`, used only by the encoder's image loader |
| `docs/` | This documentation |
| `docs/superpowers/` | Dated design specs and implementation plans, kept as a record |

## Scope

Supported: Bink 1 files with the `BIKf`, `BIKg`, `BIKh`, and `BIKi`
signatures, video and audio (DCT and RDFT), up to seven audio tracks.

Not supported: Bink 2 (`KB2*`), and the older `BIKb` and `BIKk` revisions.
The open functions return `NULL` for those.

## History documents

The files under `docs/superpowers/specs` and `docs/superpowers/plans`
describe each feature as it was designed at the time. They are not updated
when the code changes. Where one of them disagrees with the pages listed
above, the pages above describe the current code.

## License

LGPL-2.1-or-later. The decoder is a port of FFmpeg's Bink code; the
attribution is in [`bink.md`](bink.md#attribution).
