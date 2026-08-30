#!/bin/sh
set -eu
PORT=${LIBREECHO_LINUX_TIMER_PORT:-18083}
URL="http://127.0.0.1:$PORT"
LOG=./build/test-linux-timers.log
./build/libreecho-web --backend linux --web-root ./web --listen "127.0.0.1:$PORT" >"$LOG" 2>&1 &
pid=$!
cleanup() {
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
i=0
while ! curl -sS "$URL/api/v1/config" >/dev/null 2>&1; do
    i=$((i + 1))
    [ "$i" -lt 30 ] || { printf '%s\n' "Linux timer validation server did not start" >&2; exit 1; }
    sleep 0.1
done
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/timers" \
    -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"seconds":4294967297}')
[ "$code" = 400 ] || { echo "FAIL: Linux overflow returned $code, expected 400"; exit 1; }
echo "Linux timer overflow validation: ok"

# GET status is deliberately a successful unavailable envelope when timerd is
# absent, so the UI can render the feature without treating it as a fault.
curl -fsS "$URL/api/v1/timers" | jq -e \
    '.ok and .data.available == false and .data.timers == [] and
     .data.ringing == 0 and .data.missed == 0' >/dev/null
echo "Linux timer unavailable status: ok"

# The Linux adapter preserves timerd's full-schedule rejection as LE_BUSY, so
# the API can distinguish capacity from an unavailable daemon.
grep -q 'no free timer slots' src/backend_linux.c
grep -q 'Timer capacity is full' src/api.c
echo "Linux timer capacity mapping: ok"
