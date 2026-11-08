#!/bin/sh
# Capture s16le PCM for the audio reference checksums.
# Prefers ffmpeg. Falls back to VLC's WAV file output; the Python
# generator skips the WAV header.
set -eu

VLC=${VLC:-/Applications/VLC.app/Contents/MacOS/VLC}
MEDIA=${BINK_TEST_MEDIA:-$HOME/Downloads}
OUT=${1:-build/reference-audio}

mkdir -p "$OUT"

capture_one() {
  name=$1
  src="$MEDIA/$name"
  if command -v ffmpeg >/dev/null 2>&1; then
    ffmpeg -y -i "$src" -vn -f s16le -acodec pcm_s16le "$OUT/$name.s16le"
    return
  fi
  rm -f "$OUT/$name.wav"
  "$VLC" -I dummy --no-video --play-and-exit \
    --sout "#transcode{acodec=s16l}:std{access=file,mux=wav,dst=$OUT/$name.wav}" \
    "$src" >/dev/null 2>&1 || true
  test -s "$OUT/$name.wav"
}

for name in logo_lucas.bik valve.bik original.bik; do
  capture_one "$name"
done

python3 tests/gen_audio_reference.py \
  "$OUT"/logo_lucas.bik.* \
  "$OUT"/valve.bik.* \
  "$OUT"/original.bik.* \
  > tests/reference_audio.h
