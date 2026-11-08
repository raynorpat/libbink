# libbink audio design

Date: 2026-11-08
Status: approved

## Intent

Play the audio that is actually stored in a Bink 1 file. Each track keeps the sample rate, the mono or stereo layout, and the codec the container recorded for it. When more than one track is enabled, they are heard together. The picture does not stop because one track's packet cannot be decoded, and neither do the other tracks.

This is phase 2 of `docs/superpowers/specs/2026-11-08-libbink-design.md`. Video decode, the container parser, and the public names already exist. Audio packets are parsed far enough to skip them. `bink_enable_audio`, `bink_get_audio`, and `bink_get_audio_size` exist and do nothing useful yet.

## Goals

- Decode Bink Audio DCT and Bink Audio RDFT into interleaved signed 16-bit PCM.
- Use the container's per-track sample rate, channel count, and codec flag. Do not assume 44100 Hz stereo DCT.
- Return each enabled track's PCM for the current frame through the existing accessors.
- Mix every enabled track in `bink_sdl_player` so they play at the same time, lined up with the picture.
- On a seek, drop sound that was already queued and continue from the frame that was landed on.
- Keep decoding the picture and the other tracks when one track's payload cannot be decoded.
- Prove the five sample files: DCT on `valve.bik`, `ATI-9700-Animusic-Movie-v1.0.bik`, and `logo_lucas.bik`; RDFT on `original.bik`; no audio on `Robotica_1080_bink_4100k.bik`.

## Non-goals

- Bink 2 audio, or any codec other than DCT (`flags & 0x1000`) and RDFT (that flag clear).
- More than the container's maximum of 7 tracks, or more than 2 channels on a track.
- A new public mixing function. The library returns one track at a time. The player mixes.
- Audio output from `bink_dump`.
- Changing video plane bytes, RGB bytes, or the existing video checksums.
- Pitch control, volume controls, or an audio-only seek API.

## Anti-goals

- A runtime dependency on FFmpeg, libsamplerate, or any other third-party library. The C standard library, including libm, is allowed. SDL stays in the player only.
- Stopping the movie because one audio payload is bad.
- Inventing a codec from the MultimediaWiki page. That page does not specify Bink Audio well enough to match the files.
- Silently playing a hard-coded rate or channel count that disagrees with the header.

## What was decided

These came from the conversation, not from the builder:

- Play each track according to the container: its rate, its mono or stereo flag, and its DCT or RDFT codec.
- Enabled tracks play at the same time.
- A payload the decoder cannot finish drops that track for that frame. The picture and the other tracks keep going.
- A seek throws away queued sound and starts again at the landed frame.
- Tracks whose sample rates differ are brought to one rate before the mix. That rate is the first enabled track's rate.
- A file with no audio track stays silent, and the video still plays.

## Constraints

- C99. The library links only the C standard library. `-Wall -Wextra` stays clean.
- LGPL-2.1-or-later. The decoder is a port of FFmpeg's `libavcodec/binkaudio.c`. The new file keeps FFmpeg's copyright header, and the README credits it.
- Public names stay as they are. No new exported functions.
- `bink_get_audio` pointers are library-owned, valid until `bink_close`, and their contents change on the next decoded frame. This matches the video accessors.
- Up to 7 tracks. Channel count is 1, or 2 when `flags & 0x2000`. Sample rate and flags are the values already stored by the container parser.
- One audio chunk per track per frame, already framed as `u32 chunk_size` plus that many bytes, before the video payload.

## Behaviour

### Enabling

`bink_enable_audio(object, track, 1)` creates that track's decoder and marks it enabled. `bink_enable_audio(object, track, 0)` disables it and discards its decoder state. Returns `-1` for a NULL handle, a track number past the tracks in the file, or an allocation failure. Returns `0` on success.

Audio stays off until that call. This matches `bink_enable_video`. Enabling a track halfway through a file starts a fresh decoder on the next decoded frame. It does not try to reconstruct overlap from frames that were skipped.

Video and audio enables are independent. Frames still advance when video is disabled. Audio for enabled tracks is still produced.

### Decoding a frame

`bink_first`, `bink_next`, and `bink_seek_keyframe` keep doing what they do for video. After the frame packet is split:

1. Decode video if video is enabled. A video failure still returns `BINK_ERROR`. Audio is not decoded for that failed call.
2. For each enabled track, decode that track's chunk.
3. A chunk whose declared size does not fit in the frame packet is a broken frame, not a broken codec. `bink_read_frame_chunk` already rejects that and the frame returns `BINK_ERROR`, because the video payload and the later tracks cannot be found.
4. A chunk that fits, but whose payload the decoder cannot finish, produces no samples for that track on this frame. The call still returns `BINK_MORE`, `BINK_LAST`, or `BINK_DONE` as the video walk requires. Other tracks are decoded as usual. "Cannot finish" means the bit reader runs out of bits, or a checked limit in the port fails. A same-length bit flip that still decodes may produce garbage PCM. That is not a failed payload. The picture still advances either way.

`bink_first` and `bink_seek_keyframe` reset every track's overlap state before decoding the landed frame. Seeking still lands on the keyframe and the caller still walks forward with `bink_next`. Audio state advances with those frames, so a walk from the keyframe matches a fresh open.

### PCM the caller sees

`bink_get_audio(object, track)` returns interleaved native-endian signed 16-bit samples for the current frame: mono is `S0 S1 ...`, stereo is `L0 R0 L1 R1 ...`. It returns NULL when the handle is NULL, the track is out of range, the track is disabled, no frame has been decoded yet, or this frame produced no samples for that track.

`bink_get_audio_size(object, track)` returns the byte length of that buffer, or `0` in every case where `bink_get_audio` returns NULL. The length may differ from frame to frame. Callers must use the size from the same frame as the pointer.

The container stores a per-track maximum decoded size in the words the parser currently skips. Read those words and keep them. The decoder's output buffer for a track is that size, and no smaller. On the sample files the first packet is the large one: 134400 bytes for `logo_lucas.bik`, 138240 for `original.bik` and `valve.bik`, and 145920 for the ATI file. Later packets are smaller. A fixed 65536-byte cap would drop frame 0 of every file that has audio, so there is no such cap. A payload that would write past the container's maximum is a failed payload for that track.

### Player

`bink_sdl_player` enables every track the file contains. It opens one SDL audio device (SDL3 if the binary was built with SDL3, otherwise SDL2) at the first enabled track's sample rate. The device is stereo if any enabled track is stereo, otherwise mono.

Before mixing, each track is converted to that rate and channel count. Mono becomes stereo by duplicating the sample. Stereo becomes mono by averaging the pair. Rate conversion is linear interpolation. Samples are summed with saturation to signed 16-bit and queued to the device.

Bink Audio delivers samples in bursts. The first packet of a file often holds many blocks, and later frames often have an empty chunk because those samples were already delivered. Empty chunks, and failed payloads, add nothing. They do not insert a video frame of silence. The samples already queued keep playing under the later pictures. Padding those holes would slide the soundtrack away from the picture.

While the device is running, the player clocks the picture from the queued audio instead of the free-running frame delay it uses today. Left and Right still seek by one second, then walk forward from the keyframe. The seek path clears the SDL audio queue before the new frames are decoded.

If the audio device cannot be opened, the player prints the SDL error, leaves the sound off, and still shows the video. A file with no tracks does not open a device.

`BINK_SDL_TEST_FRAMES` and `BINK_SDL_TEST_SEEK` stay headless. They do not require a working audio device. The test path still enables and decodes audio so a failure there is reported.

## Codec

Port FFmpeg's `libavcodec/binkaudio.c` behaviour, including both the DCT and RDFT paths, the rate-dependent transform size, the band/RLE coefficient coding, and the overlap-add from the previous block. Internal transform math may use `float`. The bytes returned to the caller are converted to interleaved signed 16-bit at the end.

`flags & 0x1000` selects DCT. Otherwise the track is RDFT. `flags & 0x2000` selects stereo. Those are the flags already stored on each `BinkAudioTrack`.

The port lives in `src/bink_audio.c` and `src/bink_audio.h`. Both library targets in `CMakeLists.txt` compile the new file. No other FFmpeg translation unit is added.

## Verification

- Build warning-clean.
- With `BINK_TEST_MEDIA` set, decode every frame of the five files with audio enabled. DCT files and `original.bik` produce a non-zero PCM size on frames that contain a payload. `Robotica_1080_bink_4100k.bik` has a zero track count. Enabling track 0 there returns `-1`, and `bink_get_audio` stays NULL.
- Compare per-frame PCM checksums for `logo_lucas.bik`, `valve.bik`, and `original.bik` with PCM captured from VLC or FFmpeg for those same frames. At least one early frame, one middle frame, and the last frame of each of those three files. A mismatch fails the test.
- Disk mode and memory mode produce the same audio checksums for `logo_lucas.bik`.
- Shorten one audio payload in a copied `logo_lucas.bik` buffer so the bit reader cannot finish that block, without moving the video payload. Video frames still decode, and that track contributes no samples for the damaged frame. A same-length single-byte flip is not this test. The test must not crash.
- A small mixer test feeds two synthetic PCM buffers of different rates and channel counts and checks the mixed result. No two-track `.bik` is required.
- Run `logo_lucas.bik` and `original.bik` under AddressSanitizer and UndefinedBehaviorSanitizer with audio enabled.
- The existing video checksum tests still pass.

Reference PCM is generated by a script in `tests/`, the same way `tests/capture_reference.sh` captures video. The script is not run by CTest. Checked-in checksums are.

## Calls made while checking the draft

These were not in the conversation. They are the builder's calls. A wrong one would have silenced the start of every soundtrack, or slid the sound away from the picture.

- The output buffer is the container's per-track maximum decoded size, which the parser must start reading. Frame 0 of the sample files is larger than 65536 bytes and must be audible.
- An empty chunk adds no samples and no silence. The player does not invent a frame of silence to fill it.
- A failed payload means the decoder cannot finish the block. The corruption test shortens a payload until the bit reader fails. A bit flip that still decodes is allowed to produce garbage PCM, and the picture still advances.

## Decisions left to the builder

- Exact overlap buffer layout, window function, and how closely the port follows FFmpeg's function boundaries.
- Whether the linear resampler lives in the player file or a static helper next to it.
- The SDL queue depth and the threshold that means "enough audio is queued to show the next frame".
- How many reference frames to checksum beyond the required early, middle, and last frames, if a shorter set does not catch a drift bug.
- The VLC or FFmpeg command line used to capture reference PCM, as long as the checksum test can be regenerated from the sample files.

## Out of scope for the plan that follows this

Do not reopen video block decoding or the public function list. The container change is limited to keeping the per-track maximum decoded size that the parser currently skips. Wire audio into the frame walk that already finds those chunks.
