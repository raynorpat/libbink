# libbink Audio Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decode each Bink 1 audio track into interleaved signed 16-bit PCM and play every enabled track together from `bink_sdl_player`.

**Architecture:** Keep the public API. Read the per-track maximum decoded size the container already stores, and port FFmpeg's `libavcodec/binkaudio.c` (DCT and RDFT) into `src/bink_audio.c`. The frame walk decodes each enabled track after video. The player mixes and queues PCM; it does not decode.

**Tech Stack:** C99, CMake 3.16+, libm, SDL2 or SDL3 for the player, VLC or FFmpeg for one-time reference PCM capture, Python 3 for checksum generation.

**Spec:** `docs/superpowers/specs/2026-11-08-libbink-audio-design.md`

## Global Constraints

- C99. The library links only the C standard library, including libm. No FFmpeg, libsamplerate, or other third-party library. SDL stays in the player.
- LGPL-2.1-or-later. `src/bink_audio.c` keeps the FFmpeg `binkaudio.c` copyright header. README credits FFmpeg.
- No new public functions. The names that already exist are `bink_enable_audio`, `bink_get_audio`, and `bink_get_audio_size`.
- `bink_get_audio` pointers are library-owned, valid until `bink_close`, and their contents change on the next decoded frame.
- At most 7 tracks. A track is stereo when `flags & 0x2000`, otherwise mono. DCT when `flags & 0x1000`, otherwise RDFT.
- One chunk per track per frame: a `u32` size, then that many payload bytes, before the video payload.
- A payload the decoder cannot finish produces no samples for that track. The frame still returns `BINK_MORE`, `BINK_LAST`, or `BINK_DONE`. Other tracks are decoded. A chunk size that does not fit in the frame packet stays a `BINK_ERROR`, because the rest of the packet cannot be found.
- Video failure still returns `BINK_ERROR` and does not decode audio for that call.
- Empty chunks add no samples and no silence, and do not reset overlap.
- `-Wall -Wextra` clean, or `/W3` on MSVC.
- Tests that only read sample files check `BINK_TEST_MEDIA` and return 77 when it is unset. Do not default it to `$HOME/Downloads`. `test_audio` always runs its synthetic half and returns 0 when the variable is unset. `test_mix` does not read sample files.
- Do not change video plane bytes, RGB bytes, or the existing video checksums.

## File Map

- `src/bink_internal.h` holds `max_decoded` and the per-track decoder pointer.
- `src/bink.c` parses `max_decoded`, enables tracks, and publishes PCM after each frame.
- `src/bink_audio.h` / `src/bink_audio.c` are the DCT and RDFT port.
- `src/tools/bink_mix.h` / `src/tools/bink_mix.c` resample and sum tracks. The library does not export them.
- `src/tools/bink_sdl_player.c` enables every track, mixes, queues, and clocks the picture.
- `tests/test_audio.c` is the container, decode, seek, and isolation test.
- `tests/test_mix.c` is the synthetic mixer test.
- `tests/capture_audio.sh` and `tests/gen_audio_reference.py` write `tests/reference_audio.h`. CTest does not run the capture.

## Review Focus

1. Audio device open fails: the player prints the SDL error and still decodes. Task 8 sets both `SDL_AUDIODRIVER` and `SDL_AUDIO_DRIVER` to `not-a-real-driver` and requires exit 0, `audio bytes` greater than 134400, and the SDL error on stderr.
2. The queued-audio wait is skipped under `BINK_SDL_TEST_FRAMES` and `BINK_SDL_TEST_SEEK`. Task 8 gives the 30-frame dummy-driver run a 10-second `execute_process` timeout.
3. No sample file has two tracks at different rates. Task 7 checks the mixer. Task 6 checks that a failed second track does not erase a good first track. Nothing checks that the player resamples a second track whose rate differs from the device. A mistake there still passes these files.

---

### Task 1: Keep each track's maximum decoded size

**Files:**
- Modify: `src/bink_internal.h`
- Modify: `src/bink.c` (the audio-descriptor loop that currently `reader_skip`s `4 * tracks` before the rate and flags)
- Test: `tests/test_audio.c`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: the container layout already parsed in `bink_parse_header`.
- Produces: `BinkAudioTrack.max_decoded` (`unsigned long`), the byte cap for that track's PCM. Tests may read it through `bink_internal.h`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_audio.c` with the `CHECK` macro from `tests/test_common.h`. Include `bink_internal.h`.

Build a one-track `BIKi` buffer the same way `tests/test_container.c` builds a header, but with `tracks = 1`, a max-decoded word of `134400` at the start of the audio descriptors, rate `44100`, flags `0x3000` (DCT and stereo), a zero track id, one frame, and a frame offset that lands on a 4-byte audio chunk plus an empty video tail. Open it with `bink_open_memory`. Assert `b->audio_tracks == 1` and `b->audio[0].max_decoded == 134400`.

When `BINK_TEST_MEDIA` is set, open these files with `bink_open_file` and assert:

| File | `audio_tracks` | `audio[0].max_decoded` |
| --- | ---: | ---: |
| `logo_lucas.bik` | 1 | 134400 |
| `valve.bik` | 1 | 138240 |
| `original.bik` | 1 | 138240 |
| `ATI-9700-Animusic-Movie-v1.0.bik` | 1 | 145920 |
| `Robotica_1080_bink_4100k.bik` | 0 | n/a |

The synthetic assertion always runs. When `BINK_TEST_MEDIA` is unset, skip the real-file assertions and return 0. Register the test in `tests/CMakeLists.txt` as `audio`, without `SKIP_RETURN_CODE`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --target test_audio && ./build/tests/test_audio`

Expected: FAIL on `max_decoded == 134400` because that field does not exist or is still zero.

- [ ] **Step 3: Store the field**

Add `unsigned long max_decoded` to `BinkAudioTrack`. Replace the skip of `4 * tracks` words before the per-track rate with a `u32` read into `audio[i].max_decoded`. Leave the rate, flags, channel derivation, and the later track-id skip as they are.

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build --target test_audio && ./build/tests/test_audio`

With media: `BINK_TEST_MEDIA="$HOME/Downloads" ./build/tests/test_audio`

Expected: PASS. `ctest --test-dir build -R '^(open|container|frames)$'` still passes.

- [ ] **Step 5: Commit**

```sh
git add src/bink_internal.h src/bink.c tests/test_audio.c tests/CMakeLists.txt
git commit -m "feat: record Bink per-track maximum decoded audio size"
```

---

### Task 2: Enable tracks and publish no PCM until a payload decodes

**Files:**
- Create: `src/bink_audio.h`
- Create: `src/bink_audio.c`
- Modify: `src/bink_internal.h`
- Modify: `src/bink.c`
- Modify: `CMakeLists.txt` (both library source lists, and `m` when not MSVC)
- Test: `tests/test_audio.c`

**Interfaces:**
- Consumes: `BinkAudioTrack.max_decoded`, `rate`, `channels`, `flags`.
- Produces:

```c
typedef struct BinkAudio BinkAudio;

int bink_audio_create(BinkAudio **out, unsigned long rate, int channels,
                      int use_dct, unsigned long max_bytes);
void bink_audio_reset(BinkAudio *audio);
void bink_audio_destroy(BinkAudio *audio);
int bink_audio_decode(BinkAudio *audio, const unsigned char *data,
                      unsigned long size);
const unsigned char *bink_audio_pcm(const BinkAudio *audio);
unsigned long bink_audio_pcm_bytes(const BinkAudio *audio);
```

`bink_audio_decode` returns 0 when `size == 0` and leaves the overlap state untouched. For a non-empty payload it may return -1 until Task 4. On -1, the published PCM length is 0 and the overlap state is unchanged. `bink_get_audio` returns NULL and `bink_get_audio_size` returns 0 unless the latest successful decode of that track produced samples.

- [ ] **Step 1: Write the failing test**

Extend `tests/test_audio.c`:

- `bink_enable_audio(NULL, 0, 1)` returns `-1`.
- On the synthetic one-track file, `bink_enable_audio(b, 1, 1)` returns `-1`. `bink_enable_audio(b, 0, 1)` returns `0`. Before `bink_first`, `bink_get_audio(b, 0)` is NULL and `bink_get_audio_size(b, 0)` is 0.
- `bink_first` on that file returns `BINK_MORE` or `BINK_LAST`, not `BINK_ERROR`. The 4-byte payload cannot be decoded, so `bink_get_audio` stays NULL.
- `bink_enable_audio(b, 0, 0)` returns `0`, and a following `bink_next` or `bink_first` still does not return `BINK_ERROR`.
- With media, `bink_enable_audio` on track 0 of `Robotica_1080_bink_4100k.bik` returns `-1`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --target test_audio && ./build/tests/test_audio`

Expected: FAIL because `bink_enable_audio` still ignores the track number and returns 0.

- [ ] **Step 3: Wire enable and the frame walk**

`bink_enable_audio(object, track, 1)` rejects a NULL object or `track >= audio_tracks` with `-1`. It creates the decoder with `use_dct = (flags & 0x1000) != 0` and `max_bytes = max_decoded`. `enable == 0` destroys that decoder.

In `bink_read_frame_chunk`, remember each payload pointer and length (`frame_data + pos + 4`, `audio_size`) while skipping the chunk. After a successful video decode in `bink_first`, `bink_next`, and `bink_seek_keyframe`, decode each enabled track. A `-1` from `bink_audio_decode` does not change the frame status. `bink_first` and `bink_seek_keyframe` call `bink_audio_reset` on every existing decoder before that decode.

Add `src/bink_audio.c` to `bink_static` and `bink_shared`. For this task the decode of a non-empty payload returns `-1` immediately, and an empty payload returns 0. `bink_audio_create` leaves the decoder in the same state as `bink_audio_reset`: the next successful block is the first block and does not overlap-add. Overlap storage can be empty until Task 4.

Link `m` on both library targets when the compiler is not MSVC.

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build --target test_audio && BINK_TEST_MEDIA="$HOME/Downloads" ctest --test-dir build -R '^(audio|frames|container)$' --output-on-failure`

Expected: PASS. `frames` still passes with audio left disabled.

- [ ] **Step 5: Commit**

```sh
git add src/bink_audio.h src/bink_audio.c src/bink_internal.h src/bink.c CMakeLists.txt tests/test_audio.c
git commit -m "feat: enable Bink audio tracks without failing the frame"
```

---

### Task 3: Capture reference PCM checksums

**Files:**
- Create: `tests/capture_audio.sh`
- Create: `tests/gen_audio_reference.py`
- Create: `tests/reference_audio.h`

**Interfaces:**
- Consumes: nothing from the decoder.
- Produces: `reference_audio[]` entries `{ file, pcm_bytes, hash, frame0_bytes, frame0_hash }` for `logo_lucas.bik`, `valve.bik`, and `original.bik`. `hash` is FNV-1a, the same constants as `fnv1a` in `tests/test_common.h`, over the whole s16le stream. `frame0_hash` is the hash of the first `frame0_bytes` of that stream. `frame0_bytes` is 134400, 138240, and 138240 for those three files.

- [ ] **Step 1: Write the generator**

`tests/capture_audio.sh` reads `BINK_TEST_MEDIA` (default `$HOME/Downloads`) and writes a raw s16le file per movie under `build/reference-audio/`. Use `ffmpeg -i FILE -vn -f s16le -acodec pcm_s16le OUT` when `ffmpeg` is on `PATH`. Otherwise use VLC's file audio output and have the Python script skip a WAV header. `tests/gen_audio_reference.py` writes `tests/reference_audio.h`.

- [ ] **Step 2: Run the capture**

Run: `BINK_TEST_MEDIA="$HOME/Downloads" tests/capture_audio.sh`

Expected: the header exists, each `pcm_bytes` is non-zero and divisible by 4 (stereo s16), and each `frame0_bytes` is smaller than `pcm_bytes`.

- [ ] **Step 3: Commit**

```sh
git add tests/capture_audio.sh tests/gen_audio_reference.py tests/reference_audio.h
git commit -m "test: capture Bink audio reference checksums"
```

---

### Task 4: Decode Bink Audio DCT

**Files:**
- Modify: `src/bink_audio.c`
- Modify: `src/bink_audio.h` only if the Task 2 signatures are not enough
- Test: `tests/test_audio.c`

**Interfaces:**
- Consumes: `bink_audio_decode(audio, payload, size)` where `payload` does not include the container's chunk-size dword.
- Produces: interleaved native-endian s16 PCM for one chunk. Stereo is `L0 R0 L1 R1`. A successful chunk replaces the published bytes. Overlap commits only when every block in the chunk succeeds.

- [ ] **Step 1: Write the failing test**

When media is set, enable audio and video on `logo_lucas.bik` and `valve.bik`. Walk every frame. Concatenate each non-zero `bink_get_audio(b, 0)` into one stream. Assert:

- After frame 0, `bink_get_audio_size(b, 0) == reference.frame0_bytes` and the FNV of those bytes equals `reference.frame0_hash`.
- At frame `frame_count / 2`, the concatenated length is greater than `frame0_bytes` and less than `reference.pcm_bytes`.
- After the last frame, the concatenated length equals `reference.pcm_bytes` and the FNV equals `reference.hash`.
- At least one frame after frame 0 returns size 0.
- `bink_get_audio` is NULL whenever the size is 0.
- The walk never returns `BINK_ERROR`.

Also assert `ATI-9700-Animusic-Movie-v1.0.bik` frame 0 size is 145920 and a full walk with video disabled does not return `BINK_ERROR`. Compare no ATI hash in this task. `logo_lucas.bik` and `valve.bik` enable video as well as audio.

- [ ] **Step 2: Run the test to verify it fails**

Run: `BINK_TEST_MEDIA="$HOME/Downloads" ./build/tests/test_audio`

Expected: FAIL because the non-empty payload still returns -1 and frame 0 size is 0.

- [ ] **Step 3: Port the DCT decoder**

Port FFmpeg `libavcodec/binkaudio.c` from current master into `src/bink_audio.c`. Fetch `libavcodec/wma.c` for `ff_wma_critical_freqs` and copy those 25 values unchanged.

Pinned choices, matching that file for `BIKf`/`BIKg`/`BIKh`/`BIKi`:

- `version_b` and `version_2` are 0. Quantizer indexes are 8 bits. Coefficients use the `get_float` path (5-bit exponent, 23-bit mantissa, sign).
- Frame length bits are 9 below 22050 Hz, 10 below 44100 Hz, and 11 otherwise. `frame_len = 1 << bits`. `overlap_len = frame_len / 16`.
- Skip the first 32 bits of the payload. Decode blocks until no bits remain. Align to the next 32-bit boundary between blocks, as `get_bits_align32` does.
- Implement a float inverse DCT that matches FFmpeg `AV_TX_FLOAT_DCT` with inverse set, length `frame_len / 2`, and scale `1 / frame_len`. Do not link libavutil.
- Emit `frame_len - overlap_len` samples per channel per block. Overlap-add the start of a block only when it is not the first block since `bink_audio_reset`. Keep the last `overlap_len` samples for the next block, and do not publish them.
- Convert with `lrintf` and clip to [-32768, 32767]. Interleave channels.
- If the next published byte would pass `max_bytes`, fail the chunk and commit neither PCM nor overlap.

`original.bik` is RDFT. Leave that path returning -1 until Task 5.

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build --target test_audio && BINK_TEST_MEDIA="$HOME/Downloads" ./build/tests/test_audio`

Expected: PASS. Then `BINK_TEST_MEDIA="$HOME/Downloads" ctest --test-dir build -R decode --output-on-failure` still passes.

- [ ] **Step 5: Commit**

```sh
git add src/bink_audio.c src/bink_audio.h tests/test_audio.c
git commit -m "feat: decode Bink Audio DCT"
```

---

### Task 5: Decode Bink Audio RDFT

**Files:**
- Modify: `src/bink_audio.c`
- Test: `tests/test_audio.c`

**Interfaces:**
- Consumes: the Task 4 decoder. `use_dct == 0` selects this path.
- Produces: the same s16 interleave as Task 4. For RDFT, FFmpeg treats the channels as one interleaved transform: multiply the rate by the channel count when building bands, set the internal channel count to 1, and add `log2(channels)` to the frame length bits. The published buffer is still real stereo or mono s16.

- [ ] **Step 1: Write the failing test**

Extend the Task 4 walk to `original.bik`. Frame 0 size is 138240 and matches `frame0_hash`. The full concatenation matches `pcm_bytes` and `hash`. The middle-frame length sits strictly between them. The walk does not return `BINK_ERROR`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `BINK_TEST_MEDIA="$HOME/Downloads" ./build/tests/test_audio`

Expected: FAIL on `original.bik` frame 0 size, which is still 0.

- [ ] **Step 3: Add the RDFT path**

Match FFmpeg's RDFT block: negate the odd coefficients from index 2, pack the Nyquist sample, and run an inverse real DFT with scale `0.5` and length `frame_len`. `root` is `2 / (sqrt(frame_len) * 32768)`. Do not apply the DCT `root` or the DCT's extra `coeffs[0] /= 0.5`.

- [ ] **Step 4: Run the test to verify it passes**

Run: `BINK_TEST_MEDIA="$HOME/Downloads" ./build/tests/test_audio && BINK_TEST_MEDIA="$HOME/Downloads" ctest --test-dir build -R '^decode$' --output-on-failure`

Expected: PASS.

- [ ] **Step 5: Commit**

```sh
git add src/bink_audio.c tests/test_audio.c
git commit -m "feat: decode Bink Audio RDFT"
```

---

### Task 6: A bad payload, a seek, and disk versus memory

**Files:**
- Modify: `src/bink.c` only if reset or per-track publish is wrong
- Modify: `src/bink_audio.c` only if a failed chunk commits overlap
- Test: `tests/test_audio.c`

**Interfaces:**
- Consumes: Task 4 PCM and `bink_audio_reset`.
- Produces: no new symbols. `bink_seek_keyframe` already resets decoders from Task 2.

- [ ] **Step 1: Write the failing test**

Add three checks to `tests/test_audio.c`, skipped with the rest of the media section when `BINK_TEST_MEDIA` is unset:

1. **Truncated payload.** Build a one-frame `BIKi` in memory with video disabled. Use `logo_lucas.bik` frame 0's real audio payload with the last 8 bytes removed, the chunk-size dword reduced by 8, and `max_decoded` 134400. Enable track 0. `bink_first` is not `BINK_ERROR`. `bink_get_audio_size` is 0.
2. **Second track.** Build a one-frame, two-track `BIKi` with video disabled. Track 0's payload is frame 0's real `logo_lucas.bik` audio payload, and its `max_decoded` is 134400. Track 1's payload is 8 zero bytes. Enable both tracks. `bink_first` is not `BINK_ERROR`. Track 0's PCM hash equals the logo frame-0 reference hash and its size is 134400. Track 1's size is 0.
3. **Seek and modes.** Open `logo_lucas.bik` in disk mode, enable audio and video, walk to frame 10, then `bink_seek_keyframe(b, 10)`. Frame 0's PCM hash equals a fresh open's frame 0 hash. Open the same file with `BINK_MODE_MEMORY` and assert the full-stream hash equals the disk-mode full-stream hash.

- [ ] **Step 2: Run the test to verify it fails**

Run: `BINK_TEST_MEDIA="$HOME/Downloads" ./build/tests/test_audio`

Expected: FAIL on the truncated payload if the short chunk still returns samples or fails the frame, or on the seek hash if overlap is not reset. If all three already pass, record that in the task commit message and do not weaken the assertions.

- [ ] **Step 3: Fix only what failed**

Keep a failed chunk from committing overlap or PCM. `bink_audio_reset` must run before the landed frame is decoded on `bink_first` and `bink_seek_keyframe`. Track buffers are independent.

- [ ] **Step 4: Run the test to verify it passes**

Run: `BINK_TEST_MEDIA="$HOME/Downloads" ctest --test-dir build -R '^(audio|seek|decode)$' --output-on-failure`

Expected: PASS.

- [ ] **Step 5: Commit**

```sh
git add src/bink.c src/bink_audio.c tests/test_audio.c
git commit -m "fix: keep a bad Bink audio payload from stopping the frame"
```

---

### Task 7: Mix tracks into one PCM stream

**Files:**
- Create: `src/tools/bink_mix.h`
- Create: `src/tools/bink_mix.c`
- Test: `tests/test_mix.c`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt` only if the mixer must be compiled into a helper target. Prefer compiling `bink_mix.c` into `test_mix` directly.

**Interfaces:**
- Consumes: interleaved s16 from Task 4.
- Produces:

```c
typedef struct {
    const int16_t *pcm;
    unsigned long samples; /* per channel */
    unsigned long rate;
    int channels;
} BinkMixTrack;

/* Write dst_frames frames of out_channels. Return 0, or -1 on a NULL
   destination or a zero rate. */
int bink_mix(const BinkMixTrack *tracks, int count,
             unsigned long out_rate, int out_channels,
             int16_t *dst, unsigned long dst_frames);
```

Linear interpolation. Mono expanded to stereo duplicates the sample. Stereo folded to mono averages the pair. Summing clips to signed 16-bit. A NULL `pcm` or zero `samples` contributes silence for that track and does not invent a replacement length.

- [ ] **Step 1: Write the failing test**

`tests/test_mix.c`:

- Mono rate 1000, samples `{0, 1000}`, mixed to rate 2000 mono for 4 frames, equals `{0, 500, 1000, 1000}`.
- Mono sample `{1000}` mixed to stereo one frame equals `{1000, 1000}`.
- Two mono rate-1000 tracks, samples `{20000}` and `{20000}`, one output frame, equal `{32767}`.
- `{100}` plus `{50}` equals `{150}`.
- A track with `pcm == NULL` does not change the other track's sample.

Register `mix` in `tests/CMakeLists.txt`. It does not use `BINK_TEST_MEDIA`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --target test_mix && ./build/tests/test_mix`

Expected: FAIL to link or compile because `bink_mix` does not exist.

- [ ] **Step 3: Implement `bink_mix`**

Put the functions in `src/tools/bink_mix.c`. Compile that file only into `test_mix` and, in Task 8, into `bink_sdl_player`.

- [ ] **Step 4: Run the test to verify it passes**

Run: `./build/tests/test_mix`

Expected: PASS.

- [ ] **Step 5: Commit**

```sh
git add src/tools/bink_mix.h src/tools/bink_mix.c tests/test_mix.c tests/CMakeLists.txt
git commit -m "feat: mix Bink audio tracks into one PCM stream"
```

---

### Task 8: Play the mix with the picture

**Files:**
- Modify: `src/tools/bink_sdl_player.c`
- Modify: `CMakeLists.txt` (add `src/tools/bink_mix.c` to `bink_sdl_player` only)
- Modify: `tests/run_sdl.cmake`

**Interfaces:**
- Consumes: `bink_enable_audio`, `bink_get_audio`, `bink_get_audio_size`, `bink_info_audio`, `bink_mix`.
- Produces: no library symbols. On stdout, when `BINK_SDL_TEST_FRAMES` is set, one line `audio bytes N` where N is the sum of `bink_get_audio_size` over the decoded frames and all tracks.

- [ ] **Step 1: Write the failing test**

In `tests/run_sdl.cmake`, after the existing 5-frame run, add three `execute_process` calls. Set `TIMEOUT 10` on each. A timeout is a failed test. Set both names of each driver variable, because SDL2 reads `SDL_AUDIODRIVER` / `SDL_VIDEODRIVER` and SDL3 reads `SDL_AUDIO_DRIVER` / `SDL_VIDEO_DRIVER`.

- `logo_lucas.bik`, both video drivers `dummy`, both audio drivers `dummy`, `BINK_SDL_TEST_FRAMES=30`. Fail if the status is non-zero or stdout does not match `audio bytes` followed by an integer greater than 134400.
- The same 30-frame command with both audio drivers set to `not-a-real-driver`. Fail if the status is non-zero. Stdout must still match `audio bytes` with an integer greater than 134400, and stderr must contain the SDL error text.
- `Robotica_1080_bink_4100k.bik`, dummy video and audio drivers, `BINK_SDL_TEST_FRAMES=1`. Fail if the status is non-zero or stdout does not match `audio bytes 0`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `BINK_TEST_MEDIA="$HOME/Downloads" ctest --test-dir build -R '^sdl$' --output-on-failure`

Expected: FAIL because the player does not print `audio bytes`.

- [ ] **Step 3: Play audio**

Enable every track after video is enabled, including on the `BINK_SDL_TEST_SEEK` path that returns before the window exists. If a track fails to enable, print to stderr and continue. Keep the existing video `SDL_Init`. Initialize audio with a separate `SDL_InitSubSystem(SDL_INIT_AUDIO)`. If that fails, print `SDL_GetError` and do not open a device.

Open one playback device at the first enabled track's rate, stereo if any enabled track is stereo, otherwise mono, native-endian signed 16-bit. If the file has no tracks, do not open a device and print `audio bytes 0` under `BINK_SDL_TEST_FRAMES`. If open fails, print `SDL_GetError` and keep showing video.

Use the queue API of the SDL this binary was built with:

- SDL2: `SDL_OpenAudioDevice` with `AUDIO_S16SYS` and a NULL callback, `SDL_PauseAudioDevice(dev, 0)`, `SDL_QueueAudio`, `SDL_GetQueuedAudioSize`, `SDL_ClearQueuedAudio`.
- SDL3: `SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL)` with `SDL_AUDIO_S16`, then `SDL_ResumeAudioStreamDevice` because that open starts paused. Queue with `SDL_PutAudioStreamData`. The queued byte count is `SDL_GetAudioStreamQueued`. Clear with `SDL_ClearAudioStream`.

Each presented frame, build one `BinkMixTrack` per enabled track. `samples` is `bink_get_audio_size / (channels * sizeof(int16_t))`. `rate` and `channels` are that track's own values. `pcm` is NULL when the size is 0. Mix to the device rate and channel count for as many frames as the longest converted track produces, and queue those bytes. Do not queue silence for an empty or failed track. Call `bink_mix` even when there is only one track. While the device is open and neither test env is set, delay the next picture while the queued byte count is greater than two video frames of PCM (`rate * channels * 2 * usf / 1000000 * 2`). Under `BINK_SDL_TEST_FRAMES` or `BINK_SDL_TEST_SEEK`, do not wait on the queue.

`seek_to_frame` clears the device queue before decoding the landed frame. If no device is open, that clear is a no-op. Left and Right keep seeking one second and then walking forward.

Delete the "audio decoding is not implemented" message.

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build --target bink_sdl_player && BINK_TEST_MEDIA="$HOME/Downloads" ctest --test-dir build -R '^sdl$' --output-on-failure`

Expected: PASS, including the existing 5-frame and seek runs. Repeat the `sdl` test in `build-sdl2` if that build directory exists.

- [ ] **Step 5: Commit**

```sh
git add src/tools/bink_sdl_player.c src/tools/bink_mix.c CMakeLists.txt tests/run_sdl.cmake
git commit -m "feat: play Bink audio tracks in the SDL player"
```

---

### Task 9: Document audio and re-run the sanitizer suite

**Files:**
- Modify: `README.md`
- Modify: `docs/usage.md`
- Modify: `docs/bink.md`

**Interfaces:**
- Consumes: the behaviour Tasks 1–8 shipped.
- Produces: docs that no longer say audio decode is missing.

- [ ] **Step 1: Update the docs**

`README.md`: audio is decoded when the track is enabled. The player mixes every track. DCT and RDFT are both supported. A payload the decoder cannot finish drops that track for the frame.

`docs/usage.md`: replace the "does nothing" and "always returns NULL" sentences. Document enable, the s16 layout, ownership, empty chunks, seek resetting overlap, and that video still returns `BINK_ERROR` on a video failure. Document the player clock and the `audio bytes` test line.

`docs/bink.md`: the maximum-decoded-size words are read, not skipped. Name DCT versus RDFT, the payload's leading 32 skipped bits, overlap, and the empty-chunk rule. Credit FFmpeg's `binkaudio.c`.

- [ ] **Step 2: Run the full suite and sanitizers**

```sh
cmake --build build
BINK_TEST_MEDIA="$HOME/Downloads" ctest --test-dir build --output-on-failure

cmake -S . -B build-asan -DBINK_BUILD_TOOLS=ON -DBINK_BUILD_TESTS=ON \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cmake --build build-asan
BINK_TEST_MEDIA="$HOME/Downloads" ctest --test-dir build-asan -R '^(audio|mix|decode)$' --output-on-failure
```

Expected: all pass, with no sanitizer report. `logo_lucas.bik` and `original.bik` are inside `audio`.

- [ ] **Step 3: Commit**

```sh
git add README.md docs/usage.md docs/bink.md
git commit -m "docs: describe Bink audio decode and playback"
```

---

## Self-Review Notes

- Spec coverage: max decoded size (Task 1), enable and independent video (Task 2), DCT (Task 4), RDFT (Task 5), truncated payload, second-track isolation, seek reset, disk/memory (Task 6), mixer (Task 7), player clock, queue clear, device-open failure, a file with no tracks (Task 8), docs and ASan (Task 9), reference checksums (Task 3).
- Empty chunks are pinned by the full-stream length: padding silence would make `logo_lucas.bik` longer than the reference.
- The 65536-byte cap is not reintroduced. Frame 0 sizes are the container maxima.
- `bink_dump` is unchanged.
