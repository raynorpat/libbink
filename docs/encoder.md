# Encoder

`libbink` writes Bink 1 files with the `BIKi` signature. The encoder is a
static library, `bink_encoder` (archive `libbinkenc`, CMake alias
`Bink::encoder`). The public header is `bink_encoder.h`. It links the
decoder and shares the bit writer, Huffman tables, plane geometry, and
audio block parameters with it, so the format rules live in one place.
`docs/bink.md` describes the container. This page describes what the
encoder chooses to write.

## Building

`BINK_BUILD_ENCODER` defaults to `ON`. `BINK_BUILD_TOOLS=ON` forces it on
and also builds `bink_build`.

```sh
cmake -S . -B build -DBINK_BUILD_TOOLS=ON
cmake --build build
./build/bink_build --help
```

Install publishes `libbinkenc.a` and `bink_encoder.h` next to `libbink`.

## What a file contains

The signature is `BIKi`. Versions before `i` store colors with a signed
bias; `i` stores the byte as written.

Video is lossless on the encoder's own YUV planes, not on the source RGB.
The conversion is the BT.601 studio matrix the decoder inverts:

- `Y = clamp(((66R + 129G + 25B + 128) >> 8) + 16)`
- `U = clamp(((-38R - 74G + 112B + 128) >> 8) + 128)`
- `V = clamp(((112R - 94G - 18B + 128) >> 8) + 128)`

Chroma is 4:2:0. Each chroma sample is the average of the converted
samples in the existing 2x2, not a conversion of the average RGB. That
step is lossy against the original RGB. `--verify` converts again with
the same function and compares the visible Y, U, and V samples.

Blocks are 8x8. The encoder uses fill (one color), pattern (two colors),
raw, and on delta frames skip and motion. Motion offsets are limited to
-15..15 and to the decoder's reference window. There is no alpha, no gray
flag, and no scaled, DCT, residue, or run blocks.

Frame 0 is always a keyframe. `--keyframe-interval N` also marks frames
`N`, `2N`, ... . `N` of 0 marks only frame 0. Keyframes do not use skip
or motion.

Each audio track is Bink DCT (`flags` `0x1000`, plus `0x2000` when stereo).
`bink_build` writes one track. The library accepts up to seven.
Each coefficient block stores a DC value and zero AC coefficients, aligned
to 32 bits. Silence stays exact zeros. A constant tone decodes near that
level; it is not a sample-exact copy of the PCM. RDFT is not written.
The sample rate must fit in the header's 16-bit field (1..65535). A
24-bit WAV is reduced to 16-bit, and the loader prints
`warning: 24-bit WAV reduced to 16-bit`.

## bink_build

```text
bink_build <input> [-o output.bik] [options]
```

`<input>` is a directory of `.ppm` (P6), `.bmp`, `.png`, `.jpg`, or
`.jpeg` frames, or one such image. Names sort in natural order
(`frame2` before `frame10`). Frames are loaded when they are encoded.
A single image is one frame. The default output name is `<basename>.bik`.

BMP, PNG, and JPEG are decoded with stb_image 2.30 (public domain),
vendored at `third_party/stb/stb_image.h` and not installed. They come
back as packed RGB24. Alpha is discarded. BMP RLE is rejected. JPEG is
lossy, so `--verify` compares the decoded JPEG pixels, not the image
that was compressed into that file.

| Option | Meaning |
| --- | --- |
| `-o`, `--output FILE` | Output path |
| `--audio FILE.wav` | PCM WAV for the image input (8, 16, or 24-bit, mono or stereo) |
| `--fps N[/D]` | Frame rate, default `15` |
| `--keyframe-interval N` | Default `150`. `0` marks only frame 0 |
| `--motion-range N` | Search radius `0..15`, default `8`. `0` disables motion |
| `--verify` | Decode and compare visible YUV planes and audio byte counts |
| `--verbose`, `--quiet` | Progress on stderr |
| `-h`, `--help`, `--version` | Usage and `bink_build 0.1.0` |

Exit codes match the library status values: `0` ok, `1` usage, `2` input,
`3` encode, `4` verify. Encode failures delete the output. Verify
failures leave it in place.

Audio shorter than the video is padded with silence. On the last frame the
encoder also covers any samples past the frame grid. 8-bit WAV is
unsigned and expanded to signed 16-bit. 24-bit is rounded to 16-bit.

## C API

```c
#include <bink_encoder.h>

bink_enc_source source;
bink_enc_options options;
char err[256];

bink_enc_options_default(&options);
options.verify = 1;
/* source.read_frame fills packed RGB and must be repeatable.
   source.read_audio, if set, fills interleaved host-endian s16. */
if (bink_enc_encode(&source, &options, "out.bik", err, sizeof(err)) != BINK_ENC_OK)
    return 1;
```

`bink_enc_options_default` sets a keyframe interval of 150 and a motion
range of 8. `read_frame` and `read_audio` may be called again when
`verify` is set, so they cannot consume their input.

`source.audio[n].bits` is 16. Up to 7 tracks are accepted. Width, height,
and frame count must be inside the decoder limits (7680x4800, one million
frames). The file is written with `fseek`, so it has the same 2GB ceiling
as the decoder. Frame packets start on even offsets.

Image and WAV loaders used by `bink_build` are `bink_enc_image_load` and
`bink_enc_wav_load` in the encoder sources. They are not installed headers.

## Tests

`ctest` runs `test_encoder` when the encoder is built. It round-trips
solid, pattern, raw, odd-sized, skip, and motion frames, silence and a
constant tone, and the PPM, BMP, PNG, JPEG, and WAV loaders. With tools
enabled it also checks `bink_build --help` and natural filename order.
