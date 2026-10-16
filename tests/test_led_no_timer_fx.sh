#!/bin/sh
# Focused regression harness for UI#65: the legacy v1 music path must not start
# timer- or transient-driven major FX, while keeping compatible spectrum, beat
# and priority behaviour.
#
# Builds the real ledd.c into the test translation unit (which #includes
# ledd.c) and runs it.  This harness is self-contained so the parent can wire
# it into the shared runner/Makefile, which are owned elsewhere.
#
# Usage: sh tests/test_led_no_timer_fx.sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
CC=${CC:-cc}
OUT=build/led-core
mkdir -p "$OUT"

WARN="-Wall -Wextra -Wpedantic"
CFLAGS="-std=c99 $WARN -O2 -D_POSIX_C_SOURCE=200809L -Isrc -Isrc/adapter"

# ledd-level: the test translation unit #includes ledd.c itself, so ledd.c is
# NOT compiled separately (that would duplicate main).  The handler's own
# dependencies are linked as objects.
LEDD_DEPS="src/adapter/adapter_server.c src/log.c \
src/adapter/led_output.c src/adapter/music_visualizer_protocol.c \
src/adapter/led_music_director.c src/adapter/led_music_render.c"

# shellcheck disable=SC2086
$CC $CFLAGS -o "$OUT/test-led-no-timer-fx" \
    tests/test_led_no_timer_fx.c $LEDD_DEPS
"$OUT/test-led-no-timer-fx"

echo "LED no-timer-fx regression: ok"
