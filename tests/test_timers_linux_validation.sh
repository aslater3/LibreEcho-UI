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
