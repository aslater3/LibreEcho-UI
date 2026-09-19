#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
${CC:-cc} -std=c99 -O0 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -Isrc -Isrc/adapter tests/test_live_realtime_interrupt.c \
    src/adapter/ws_client.c src/adapter/live_b64.c src/json.c \
    -Wl,--gc-sections -o "$work/test"
timeout 10 "$work/test"
