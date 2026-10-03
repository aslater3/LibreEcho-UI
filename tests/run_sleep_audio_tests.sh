#!/bin/sh
# Host tests for the audiod sleep/nursery source.
#
#   sh tests/run_sleep_audio_tests.sh
#
# Runs the synthesis unit (tests/test_sleep_generator.c), the real daemon
# lifecycle test (tests/test_sleep_audio_lifecycle.c, which compiles the
# production audiod.c in and drives its request handler against a media-bus
# file), a strict compile of audiod.c on its own, and -- when already built --
# the existing narrow audiod review test.  It needs no ALSA card.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/le-sleep-tests.XXXXXX")
BUS=/tmp/libreecho-sleep-lifecycle-bus.pcm
trap 'rm -rf "$WORK"; rm -f "$BUS"' EXIT

CC=${CC:-cc}
CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Werror"
AUDIOD_SRC="src/adapter/adapter_client.c src/adapter/adapter_server.c src/log.c"

echo "sleep audio: building synthesis unit"
"$CC" $CFLAGS -Isrc -Isrc/adapter tests/test_sleep_generator.c -lm -o "$WORK/sleep_generator"
"$WORK/sleep_generator"

echo "sleep audio: building daemon lifecycle test"
"$CC" -D_POSIX_C_SOURCE=200809L $CFLAGS -Isrc -Isrc/adapter \
    tests/test_sleep_audio_lifecycle.c $AUDIOD_SRC -lm -o "$WORK/sleep_lifecycle"
"$WORK/sleep_lifecycle"

echo "sleep audio: strict-compiling audiod.c"
"$CC" -D_POSIX_C_SOURCE=200809L $CFLAGS -Isrc -Isrc/adapter \
    -c src/adapter/audiod.c -o "$WORK/audiod.o"

if [ -x "build/test-audiod-review" ]; then
    echo "sleep audio: running existing narrow audiod review test"
    ./build/test-audiod-review
fi

echo "sleep audio tests: ok"
