#!/bin/sh
# Focused LED unit and ledd-level test harness (#66, #110, #101, #64, #65).
#
# Builds and runs every focused LED test against the real production units.
# This is the executable evidence the LED worker hands to the integration
# worker, which wires the same commands into tests/run_tests.sh and Makefile
# (those shared files are owned elsewhere).
#
# Usage: sh tests/test_led_core.sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
CC=${CC:-cc}
OUT=build/led-core
mkdir -p "$OUT"

WARN="-Wall -Wextra -Wpedantic"
CFLAGS="-std=c99 $WARN -O2 -D_POSIX_C_SOURCE=200809L -Isrc -Isrc/adapter"

# Portable units (no daemon).
LED_OUTPUT="src/adapter/led_output.c"
PROTO="src/adapter/music_visualizer_protocol.c"
DIRECTOR="src/adapter/led_music_director.c"
RENDER="src/adapter/led_music_render.c"

# ledd-level: the test translation unit #includes ledd.c itself, so ledd.c is
# NOT compiled separately (that would duplicate main).  The handler's own
# dependencies are linked as objects.
LEDD_DEPS="src/adapter/adapter_server.c src/log.c \
$LED_OUTPUT $PROTO $DIRECTOR $RENDER"

build_and_run() {
    name=$1
    shift
    printf 'building %s\n' "$name"
    # shellcheck disable=SC2086
    $CC $CFLAGS -o "$OUT/$name" "$@"
    "$OUT/$name"
}

echo "== LED unit tests =="
build_and_run test-led-output-core tests/test_led_output_core.c $LED_OUTPUT
build_and_run test-music-viz-protocol tests/test_music_visualizer_protocol.c $PROTO
build_and_run test-led-music-director tests/test_led_music_director.c \
    $DIRECTOR $RENDER $LED_OUTPUT $PROTO
build_and_run test-led-music-render-core tests/test_led_music_render_core.c \
    $RENDER $DIRECTOR $LED_OUTPUT $PROTO

echo "== LED ledd-level tests =="
build_and_run test-led-daemon-core tests/test_led_daemon_core.c $LEDD_DEPS
build_and_run test-led-night-review tests/test_led_night_review.c $LEDD_DEPS
build_and_run test-wake-led-profile tests/test_wake_led_profile.c $LEDD_DEPS

echo "LED focused suite: ok"
