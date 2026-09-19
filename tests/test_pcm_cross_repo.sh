#!/bin/sh
# Optional paired-source test: no hardware, network, credentials or device paths.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
platform=${1:?usage: sh tests/test_pcm_cross_repo.sh PATH_TO_PLATFORM_CHECKOUT}
audio="$platform/tools/mt8163-arm32/airplay"
cmp "$root/src/adapter/pcm_stream_protocol.h" "$audio/pcm_stream_protocol.h"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
${CC:-cc} -std=c99 -Wall -Wextra -Werror -I"$root/src/adapter" -I"$audio" \
  "$root/tests/test_pcm_cross_repo.c" "$root/src/adapter/live_audio_out.c" \
  "$root/src/adapter/radio_resample.c" "$root/src/adapter/playback_status_client.c" \
  "$audio/pcm_stream_server.c" -lm -o "$work/paired"
timeout 10 "$work/paired"
