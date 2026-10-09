#!/bin/sh
# Regression harness: the additive sleep-noise "heartbeat" source must not
# overflow the 8-byte legacy noise_colour alias in the mock backend, which used
# to abort the web daemon on the fortify strcpy interceptor.
#
# Built with _FORTIFY_SOURCE=2 so the pre-fix strcpy() is caught exactly as it
# is in the shipped daemon build. Self-contained, so the shared runner/Makefile
# (owned elsewhere) can wire it in.
#
# Usage: sh tests/test_feature_batch_noise_heartbeat.sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
CC=${CC:-cc}
OUT=build/noise-heartbeat
mkdir -p "$OUT"

WARN="-Wall -Wextra -Wpedantic"
CFLAGS="-std=c99 $WARN -O2 -D_FORTIFY_SOURCE=2 -D_POSIX_C_SOURCE=200809L -Isrc -Isrc/adapter"

# shellcheck disable=SC2086
$CC $CFLAGS -o "$OUT/test-noise-heartbeat" \
    tests/test_feature_batch_noise_heartbeat.c \
    src/backend_mock.c src/config_store.c src/json.c src/log.c

"$OUT/test-noise-heartbeat"
echo "mock noise heartbeat regression: ok"
