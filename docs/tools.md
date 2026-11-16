# Tools and players

Reference for the programs built with `-DBINK_BUILD_TOOLS=ON`: what each
one needs, its arguments, keys, output, and exit codes. The players are
examples of the decoder API, not finished media players.

## Which programs get built

| Program | Built when |
| --- | --- |
| `bink_dump` | Always |
| `bink_build` | Always |
| `bink_sdl_player` | An SDL3 or SDL2 CMake package is found |
| `bink_sdlgl_player` | An SDL3 or SDL2 CMake package is found |
| `bink_sdlvulkan_player` | SDL, the Vulkan headers and loader, and shaderc are found |
| `bink_sdlmetal_player` | SDL is found and the host is macOS |
| `bink_d3d9_player`, `bink_d3d11_player` | The target is Windows |

`cmake --install` installs `bink_dump`, `bink_build`, and
`bink_sdl_player`. The other players stay in the build directory.

CMake prints which optional players it configured. If one is missing,
see [Troubleshooting](#troubleshooting).

## bink_dump

Decodes a file without a display and writes each frame as an image.

```sh
bink_dump [--frames N] <video.bik> [outdir]
```

- `--frames N` stops after `N` frames. Without it every frame is decoded.
- `outdir` defaults to the current directory and must already exist.

Output on stdout:

```text
video.bik: 640x480, 300 frames, 30.000 fps
frame 0 y=... u=... v=...
frame 1 y=... u=... v=...
```

The three numbers are 64-bit FNV-1a hashes of the visible Y, U, and V
planes, the same hash the test references use. Each frame is also written
as `frame_%05lu.ppm` (binary PPM, RGB24).

Exit codes: `0` success, `1` open, decode, or write failure, `2` usage.

## bink_build

Encodes a directory of images, or one image, into a `BIKi` file with
optional WAV audio.

```sh
bink_build frames/ -o out.bik --fps 30 --audio sound.wav --verify
```

The options, input formats, and exit codes are documented in
[`encoder.md`](encoder.md#bink_build).

## Players

Every player takes exactly one argument, the file to play:

```sh
bink_sdlgl_player <video.bik>
```

### Keys

| Key | Action | Players |
| --- | --- | --- |
| Esc, Q | Quit | All |
| Space | Pause the picture and the sound | All |
| Left, Right | Seek one second back or forward | All |
| M | Switch between presentation and interpolation | All except `bink_sdl_player` |

A seek decodes forward from the nearest keyframe to the requested frame
and rebuilds the audio queue from that picture's time. A seek before the
start lands on frame 0; a seek past the end lands on the last frame. In
a file whose only keyframe is frame 0, a seek costs a decode of every
frame up to the target.

### What each player does

| Player | Window and audio | Rendering |
| --- | --- | --- |
| `bink_sdl_player` | SDL, resizable | SDL's 2D renderer with a YUV texture, letterboxed. No shaders |
| `bink_sdlgl_player` | SDL | OpenGL 3.2 core, GLSL 150 |
| `bink_sdlmetal_player` | SDL | Metal, macOS only |
| `bink_sdlvulkan_player` | SDL | Vulkan; GLSL compiled to SPIR-V by shaderc at start-up. On macOS it runs through MoltenVK |
| `bink_d3d9_player` | Win32 window, `waveOut` | Direct3D 9, HLSL compiled at start-up |
| `bink_d3d11_player` | Win32 window, `waveOut` | Direct3D 11, HLSL compiled at start-up |

All of them enable every audio track, resample each to the device rate
with linear interpolation, and mix them with 16-bit saturation. If the
audio device cannot be opened, the player prints the error and plays the
picture without sound.

### Presentation and interpolation

The GPU players have two shaders:

- **Presentation** converts the current frame's Y, U, and V textures to
  RGB with the BT.601 limited-range matrix. This is the default.
- **Interpolation** draws frames between the decoded ones. It warps the
  current frame forward and the next frame backward along the decoder's
  per-block motion vectors and blends the two by how far the display
  time is between them. Motion looks smoother than the file's frame
  rate.

Interpolation needs a next frame and a motion field. When the decoder
has none, the player prints `No motion field; interpolation off.` and
returns to presentation. On the last frame there is no next frame, so
the current frame is drawn as is.

The motion vectors are the ones the encoder chose for compression, not
true optical flow. Blocks coded without motion have a zero vector, so
they cross-fade instead of moving.

### Exit behaviour

A player exits with `0` when the file ends or the user quits. It exits
with `1`, after printing the reason to stderr, when the file cannot be
opened, the window or renderer cannot be created, rendering fails, or a
frame fails to decode during playback (`Decoding failed; stopping.`).

### Start-up output

```text
Opened file video.bik
Width: 640
Height: 480
Frames: 300
FPS: 30.000000
```

## Environment variables

These exist for automated checks. None is needed for normal use.

### GL, Metal, and Vulkan players

| Variable | Effect |
| --- | --- |
| `BINK_PLAYER_TEST_FRAMES=N` | Exit with `0` after drawing `N` times |
| `BINK_PLAYER_SHADER=interp` | Start in interpolation mode |
| `BINK_PLAYER_T=0..1` | Hold the interpolation position at this value instead of following the clock |
| `BINK_PLAYER_TEST_SEEK=N` | Seek to frame `N` before the first draw |
| `BINK_PLAYER_DUMP=<file.ppm>` | Read the first drawn frame back from the GPU, write it as a PPM, and print how far it is from the decoder's RGB |
| `BINK_VULKAN_LIBRARY=<path>` | Vulkan player only: the Vulkan loader library to load when SDL cannot find one |

`BINK_PLAYER_DUMP` prints one line:

```text
dump mean abs diff 0.08, worst 2
```

The comparison target is the current frame, or the next frame when
`BINK_PLAYER_T` is `0.5` or more. With both `BINK_PLAYER_DUMP` and
`BINK_PLAYER_T` set, the player switches to interpolation and exits
after the dump.

### bink_sdl_player

| Variable | Effect |
| --- | --- |
| `BINK_SDL_TEST_FRAMES=N` | Decode `N` frames, print `audio bytes <total>`, and exit with `0`. Falls back to a decode loop with no window if the window cannot be created |
| `BINK_SDL_TEST_SEEK=N` | Seek to frame `N`, print `seeked frame N`, and exit |

Both work with SDL's dummy drivers:

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy BINK_SDL_TEST_FRAMES=5 \
  ./build/bink_sdl_player <video.bik>
```

### Direct3D players

| Variable | Effect |
| --- | --- |
| `BINK_D3D_TEST_FRAMES=N` | Exit with `0` after `N` frames |
| `BINK_D3D_SHADER=present\|interp` | The starting shader |

## Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| CMake warns `SDL2/SDL3 not found; skipping bink_sdl_player.` | No SDL CMake package was found. Install SDL's development package, or pass `-DCMAKE_PREFIX_PATH=<sdl prefix>` |
| CMake prints `Vulkan or shaderc not found; skipping bink_sdlvulkan_player` | Install the Vulkan loader and headers and shaderc. macOS: `brew install vulkan-loader molten-vk shaderc`. Debian or Ubuntu: `libvulkan-dev` and `libshaderc-dev`. Windows: the Vulkan SDK |
| CMake prints `Cross-compiling without an SDL package path: skipping the SDL players.` | A cross build ignores the host's SDL. Pass `-DSDL3_DIR=<dir>` or `-DSDL2_DIR=<dir>` for the target |
| `Cannot load Vulkan; set BINK_VULKAN_LIBRARY to the loader.` | SDL could not find the Vulkan loader library. Set `BINK_VULKAN_LIBRARY` to its full path, for example `"$(brew --prefix vulkan-loader)/lib/libvulkan.dylib"` on macOS |
| `Errors encountered opening <file>, exiting.` | The file is missing, malformed, or an unsupported revision (Bink 2, `BIKb`, `BIKk`). For an unsupported revision or an unknown signature the library also prints a `libbink::` line on stderr |
| A player has no sound | The file has no audio tracks, or the device failed to open. In the second case the player prints the device error at start-up |
| `No motion field; interpolation off.` | The decoder had no motion vectors for the next frame. Presentation mode continues to work |

## Related

- [`usage.md`](usage.md): the decoder API the players are built on.
- [`architecture.md`](architecture.md#tools-and-players-srctools): how
  the player sources are layered, and how to add a renderer.
- [`development.md`](development.md#checking-a-renderer): using the dump
  hook to check a renderer.
