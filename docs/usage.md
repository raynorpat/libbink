# libbink usage

## Requirements

- C99 compiler
- CMake 3.16 or newer
- SDL3 or SDL2 only for the optional player example

## Linking

Link the shared library (`bink_shared`) or the static library
(`bink_static`) and include `bink.h`:

```sh
cc -I/path/to/libbink/src player.c -L/path/to/libbink/build -lbink
```

When both libraries are enabled, the CMake alias `Bink::bink` refers to the
shared library; otherwise it refers to the static library.

```cmake
add_executable(player player.c)
target_link_libraries(player PRIVATE Bink::bink)
```

## Quick start

```c
#include <bink.h>
#include <stdio.h>

int main(void)
{
    bink b = bink_open_file("video.bik", BINK_MODE_DISK);
    unsigned long frames = 0;
    double usf = 0.0;
    unsigned long w = 0, h = 0;

    if (!b)
        return 1;

    bink_info_all(b, NULL, &frames, &usf);
    bink_info_video(b, &w, &h, NULL);
    printf("%lux%lu, %lu frames, %.2f fps\n", w, h, frames,
           usf > 0.0 ? 1000000.0 / usf : 0.0);

    bink_enable_video(b, 1);
    {
        char status = bink_first(b);

        if (status == BINK_ERROR) {
            bink_close(b);
            return 1;
        }
        while (status == BINK_MORE || status == BINK_LAST) {
            /* bink_get_plane(b, BINK_PLANE_Y), bink_get_video(b), ... */
            status = bink_next(b);
        }
    }

    bink_close(b);
    return 0;
}
```

## Complete example

```c
#include <bink.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    bink b;
    unsigned long frame = 0;
    unsigned long count = 0;
    char status;

    if (argc != 2)
        return 2;

    b = bink_open_file(argv[1], BINK_MODE_DISK);
    if (!b)
        return 1;

    if (bink_enable_video(b, 1) != 0) {
        bink_close(b);
        return 1;
    }

    status = bink_first(b);
    while (status == BINK_MORE || status == BINK_LAST) {
        const unsigned char *rgb;
        int stride;

        bink_info_all(b, &frame, &count, NULL);
        rgb = bink_get_video(b);
        stride = bink_get_stride(b, BINK_PLANE_Y);
        if (rgb == NULL || stride <= 0) {
            status = BINK_ERROR;
            break;
        }
        /* Use frame, rgb, and the Y/U/V planes here. */
        status = bink_next(b);
    }

    bink_close(b);
    return status == BINK_ERROR ? 1 : 0;
}
```

## API reference

### Opening and closing

```c
bink bink_open_file(const char *filename, unsigned char mode);
bink bink_open_filepointer(FILE *file, unsigned char mode);
bink bink_open_memory(const unsigned char *buffer, unsigned long size);
void bink_close(bink object);
```

- `bink_open_file` opens a file from disk. `BINK_MODE_DISK` reads frames on
  demand; `BINK_MODE_MEMORY` loads the whole file into memory first.
- `bink_open_filepointer` takes ownership of `FILE *file`: the library
  closes it, including in `BINK_MODE_MEMORY`, where the contents are copied
  into memory before the stream is closed.
- `bink_open_memory` copies `size` bytes from `buffer`; the caller keeps
  ownership of its buffer and may free it immediately after the call.
- All open functions return `NULL` on failure, including unsupported Bink
  versions (`BIKb`, `BIKk`, Bink 2 `KB2*`) and malformed headers.
- `bink_close(NULL)` is safe.

### File information

```c
char bink_info_all(const bink object, unsigned long *frame,
                   unsigned long *frame_count, double *usf);
char bink_info_video(const bink object, unsigned long *w, unsigned long *h,
                     unsigned long *flags);
char bink_info_audio(const bink object, unsigned char *track_mask,
                     unsigned char channels[7], unsigned char bitdepth[7],
                     unsigned long audio_rate[7]);
```

All three return `0` on success and `-1` when `object` is `NULL`. Any output
pointer may be `NULL`.

- `frame` is the current 0-based frame index.
- `frame_count` is the total number of frames.
- `usf` is the frame duration in microseconds (fps denominator times
  1,000,000 divided by the fps numerator), matching libsmacker's convention.
- `flags` is the Bink video flags word. `BINK_FLAG_ALPHA` (`0x00100000`)
  means an alpha plane is present; `BINK_FLAG_GRAY` (`0x00020000`) marks
  grayscale content.
- `track_mask` has bit `i` set when audio track `i` exists. `channels` is
  1 or 2, `bitdepth` is 16, and `audio_rate[i]` is the stream sample rate.

### Enabling tracks

```c
char bink_enable_video(bink object, unsigned char enable);
char bink_enable_audio(bink object, unsigned char track,
                       unsigned char enable);
```

`bink_enable_video(b, 1)` allocates the decoder state and frame buffers;
`bink_enable_video(b, 0)` stops decoding. The first enable may fail with
`-1` on allocation failure.

`bink_enable_audio(b, track, 1)` creates that track's decoder and returns
`0`. `bink_enable_audio(b, track, 0)` destroys it. A `NULL` handle or a
track index past the file's track count returns `-1`. Video and audio
enables are independent. Enabling a track after playback has started
starts a fresh decoder, with no overlap carried in from earlier frames.

### Frame iteration and seeking

```c
char bink_first(bink object);
char bink_next(bink object);
char bink_seek_keyframe(bink object, unsigned long frame);
```

`bink_first` rewinds to frame 0 and decodes it. `bink_next` advances one
frame and decodes it. Both return one of:

- `BINK_MORE` (`1`): another frame can be decoded
- `BINK_LAST` (`2`): the frame just decoded is the last one
- `BINK_DONE` (`0`): `bink_next` was called past the last frame
- `BINK_ERROR` (`-1`): open, enable, or decode failure

`bink_seek_keyframe` clamps `frame` to the last frame, walks back to the
nearest frame whose container index entry is flagged as a keyframe, resets
the decoder, and decodes that keyframe. It returns `0` on success and `-1`
on failure. Because the decoder has no Bink 1 intra-frame state, playback
after a seek starts from the keyframe itself; call `bink_next` to advance.
`bink_first` and `bink_seek_keyframe` also clear every enabled track's
overlap before the landed frame is decoded. `bink_next` keeps that overlap
and advances it with the picture. A video decode failure still returns
`BINK_ERROR` and does not decode or replace the published audio. An audio
payload the decoder cannot finish does not fail the frame.

All five reference files in the test suite mark only frame 0 as a keyframe,
so every seek lands on frame 0 for those files.

### Frame data access

```c
const unsigned char *bink_get_video(const bink object);
const unsigned char *bink_get_plane(const bink object, int plane);
int bink_get_stride(const bink object, int plane);
const unsigned char *bink_get_audio(const bink object, unsigned char track);
unsigned long bink_get_audio_size(const bink object, unsigned char track);
```

`bink_get_plane` returns a decoded plane or `NULL` if video is disabled,
decoding has not happened yet, the plane does not exist, or `plane` is not
`BINK_PLANE_Y`, `BINK_PLANE_U`, `BINK_PLANE_V`, or `BINK_PLANE_A`.

Plane layouts:

| Plane | Size | Notes |
| --- | --- | --- |
| `BINK_PLANE_Y` | `width x height` | 8-bit luma |
| `BINK_PLANE_U` | `ceil(width/2) x ceil(height/2)` | 8-bit chroma |
| `BINK_PLANE_V` | `ceil(width/2) x ceil(height/2)` | 8-bit chroma |
| `BINK_PLANE_A` | `width x height` | present when `BINK_FLAG_ALPHA` |

Rows are top-down. `bink_get_stride` returns the row stride in bytes; it is
aligned to 32 bytes and may be larger than the visible row width. Use the
stride to walk rows and the visible sizes above to limit each row. Returns
`0` when video is disabled, before the first decode, or for an absent
plane.

`bink_get_video` converts the current Y/U/V planes to packed RGB24 with the
BT.601 limited-range formula, clamps each channel to 0..255, and ignores
alpha. The buffer is `width * height * 3` bytes, top-down, with three bytes
per pixel (`R`, `G`, `B`). Conversion is lazy and cached: repeated calls
return the same pointer until the next decode or seek, when the cache is
invalidated. Returns `NULL` when no frame has been decoded.

The plane pointers and RGB buffer belong to the `bink` handle. They remain
valid until the next decode, a seek, `bink_enable_video(b, 0)`, or
`bink_close`. Copy the data if you need it longer.

`bink_get_audio` returns the current frame's PCM for one enabled track:
native-endian signed 16-bit. Mono is `S0 S1 ...`. Stereo is `L0 R0 L1 R1 ...`.
`bink_get_audio_size` is the byte length of that buffer. Read both from the
same frame. The pointer belongs to the handle until the next
`bink_first`, `bink_next`, or `bink_seek_keyframe`, until that track is
disabled, or until `bink_close`.

The pointer is `NULL` and the size is `0` when the handle is `NULL`, the
track is out of range, the track is disabled, no frame has been decoded,
or this frame produced no samples. An empty chunk adds no samples and no
silence, and it does not reset overlap. A payload the decoder cannot
finish (the bit reader runs out, or a checked limit fails) does the same
for that track. The other tracks are still decoded.

## Error handling

- Open functions return `NULL`; no error code is available beyond a message
  on `stderr` for unsupported or malformed files.
- `bink_info_*` return `-1` only for a `NULL` handle.
- `bink_first`/`bink_next` return `BINK_ERROR` for decode failures.
- `bink_seek_keyframe` returns `-1` for a `NULL` handle or a failed decode.
- Corrupt bitstreams are rejected with an error rather than reading out of
  bounds. Out-of-range motion references leave the destination block
  unchanged.

## Thread safety

A `bink` handle is not thread-safe. Use one handle per thread; separate
handles may be used concurrently.

## Limitations

- Bink 1 only. Bink 2 (`KB2*`), `BIKb`, and `BIKk` are rejected.
- No alpha plane in the RGB conversion.
- No post-processing or scaling; the SDL player lets SDL scale the texture.
- Seeking is keyframe-only. These files use frame 0 as the only keyframe.

## Example programs

`bink_dump` writes PPM images and FNV-1a plane hashes without a display:

```sh
bink_dump --frames 3 video.bik outdir
```

`bink_sdl_player` plays a file in a resizable window:

```sh
bink_sdl_player video.bik
```

Keys: Esc or Q quits, Space pauses the picture and the sound, Left/Right
seeks one second. A seek clears the audio queue, walks forward from the
nearest keyframe, and queues the soundtrack from that picture's time
forward. Samples that belonged under earlier pictures are left out.

The player enables every track in the file. It opens one device at the
first enabled track's sample rate, in stereo when any enabled track is
stereo and in mono otherwise. Each track is resampled with linear
interpolation and the samples are summed with signed 16-bit saturation.
While the device is open, the next picture waits until one video frame of
PCM has been played. A burst stays queued and keeps playing under the
later pictures, including pictures whose chunk is empty. An empty chunk
adds no samples and no silence. If the queue is not draining, the picture
advances anyway so a stalled device cannot freeze the window. If the
device cannot be opened, the player prints the SDL error and the video
continues without sound. A file with no audio tracks does not open a device.

Setting `BINK_SDL_TEST_FRAMES` to a positive integer makes the player exit
successfully after decoding that many frames, which the CTest suite uses
with the SDL dummy video and audio drivers. The player prints one stdout
line, `audio bytes N`, where N is the sum of `bink_get_audio_size` over
those frames and every track. That path does not wait on the audio queue.
If window or renderer creation fails while that variable is set, the player
falls back to a headless decode loop. `BINK_SDL_TEST_SEEK` seeks to that
frame number, prints `seeked frame N`, and exits. It enables audio and does
not wait on the queue.
