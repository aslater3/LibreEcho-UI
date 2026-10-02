#!/bin/sh
set -eu
PORT=${LIBREECHO_SEND_TEST_PORT:-18094}
mkdir -p "${TMPDIR:-$PWD/build}"
ROOT=$(mktemp -d "${TMPDIR:-$PWD/build}/le-send-XXXXXX")
pid=0
printf '{}\n' >"$ROOT/config.json"
printf 'schema=1\n' >"$ROOT/config.json.setup-complete"
cleanup(){
    if [ "$pid" -gt 1 ]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
    rm -rf "$ROOT"
}
trap cleanup EXIT INT TERM
cc -D_POSIX_C_SOURCE=200809L -std=c99 -Wall -Wextra -Wpedantic -Werror tests/test_http_send_deadline.c -o "$ROOT/client"
cc -D_POSIX_C_SOURCE=200809L -DLE_HTTP_SEND_BOUNDS_TEST -std=c99 -Wall -Wextra -Wpedantic -Werror -Wno-unused-function -ffunction-sections -fdata-sections -Isrc -Isrc/adapter tests/test_http_send_deadline.c -Wl,--gc-sections -o "$ROOT/bounds"
"$ROOT/bounds"
mkdir -p "$ROOT/web/js"
cp web/js/app.js "$ROOT/web/js/app.js"
# app.js alone can fit in a host TCP send buffer. Pad the private fixture so
# this deterministically exercises backpressure without changing shipped web.
dd if=/dev/zero bs=1048576 count=16 >>"$ROOT/web/js/app.js" 2>/dev/null
./build/libreecho-web --backend mock --config "$ROOT/config.json" --mock-config ./config/mock-state.json --web-root "$ROOT/web" --listen "127.0.0.1:$PORT" --seed 42 --dev-controls >"$ROOT/server.log" 2>&1 &
pid=$!
i=0
while ! "$ROOT/client" "$PORT" --ready; do
    i=$((i+1))
    if ! kill -0 "$pid" 2>/dev/null || [ "$i" -ge 30 ]; then cat "$ROOT/server.log"; exit 1; fi
    sleep 0.1
done
"$ROOT/client" "$PORT"
