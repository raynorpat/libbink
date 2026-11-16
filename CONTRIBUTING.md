# Contributing to libbink

libbink is a C99 Bink 1 decoder, an encoder library, and example tools.
This page covers what a change needs before it is merged. The detailed
workflow is in [`docs/development.md`](docs/development.md).

## Set up

```sh
cmake -S . -B build -DBINK_BUILD_TOOLS=ON
cmake --build build
BINK_TEST_MEDIA="$HOME/Downloads" ctest --test-dir build --output-on-failure
```

The media tests need five sample `.bik` files that are not in the
repository; their names are in
[`docs/development.md`](docs/development.md#sample-media). Without
`BINK_TEST_MEDIA` the suite still passes, but the decoder has not been
run against real files.

## Reporting a bug

Include:

- the command or the API calls that show the problem, and the output;
- the platform, the compiler, and the SDL version if a player is
  involved;
- for a decoding problem, the file's first line from `bink_dump`
  (size, frame count, frame rate) and the frame number that fails. Do
  not attach copyrighted game video to a public report.

For a crash or a sanitizer report on malformed input, say how the input
was produced. These are treated as decoder bugs even when the file is
invalid.

## Making a change

1. Keep the change to one subject. Fix unrelated problems separately.
2. Add a test with the change. A bug fix needs a test that fails
   without the fix.
3. Update the documentation in the same commit when a public function,
   a tool option, an environment variable, or an exit code changes.
4. Run the checks below.

### Checks

| Check | Command |
| --- | --- |
| Build without warnings | `cmake --build build` |
| Tests with media | `BINK_TEST_MEDIA=<dir> ctest --test-dir build --output-on-failure` |
| Sanitizers, for decoder, encoder, or renderer changes | [`docs/development.md`](docs/development.md#sanitizer-build) |
| Renderer output, for player changes | [`docs/development.md`](docs/development.md#checking-a-renderer) |
| Windows sources still compile, for changes under `src/tools/` | [`docs/development.md`](docs/development.md#windows-cross-build) |

### Code

- Strict C99, no compiler extensions, `-Wall -Wextra` clean.
- The decoder library uses only the C library and `libm`.
- `bink.h` and `bink_encoder.h` are the public API. Changing either is
  an API change and needs a matching update to `docs/usage.md` or
  `docs/encoder.md`.
- Decoder behaviour follows FFmpeg's Bink implementation. Keep the
  FFmpeg copyright headers in the files derived from it.
- Do not regenerate `tests/reference_checksums.h` or
  `tests/reference_audio.h` to make a failing test pass.

The full list is under
[Conventions](docs/development.md#conventions).

### Commits

Write the subject as `area: what changed`, using the area names already
in the history (`fix:`, `feat:`, `docs:`, `encoder:`, `decoder:`,
`tools:`, `test:`, `build:`).
Use the body to say why, and what you ran to verify it, including
anything you could not run, such as a Direct3D player on a non-Windows
host.

## License

Contributions are accepted under the LGPL-2.1-or-later, the license of
the project. Code taken from another project must be compatible with it
and keep its copyright notice.
