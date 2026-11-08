#!/bin/sh
set -eu

VLC=${VLC:-/Applications/VLC.app/Contents/MacOS/VLC}
MEDIA=${BINK_TEST_MEDIA:-$HOME/Downloads}
OUT=${1:-build/reference}

mkdir -p "$OUT"
for name in \
  ATI-9700-Animusic-Movie-v1.0.bik \
  Robotica_1080_bink_4100k.bik \
  logo_lucas.bik \
  original.bik \
  valve.bik
do
  "$VLC" -I dummy --no-audio --vout yuv \
    --yuv-file "$OUT/$name.yuv" --yuv-chroma I420 --yuv-yuv4mpeg2 \
    --no-video-title-show --stop-time=0.6 --play-and-exit \
    "$MEDIA/$name" >/dev/null 2>&1
  test -s "$OUT/$name.yuv"
done

python3 tests/gen_reference.py "$OUT"/*.yuv > tests/reference_checksums.h
