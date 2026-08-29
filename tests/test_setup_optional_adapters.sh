#!/bin/sh
set -eu

PORT=${LIBREECHO_OPTIONAL_ADAPTER_PORT:-18193}
URL="http://127.0.0.1:$PORT"
TMP=$(mktemp -d)
CFG="$TMP/config.json"
LOG="$TMP/server.log"
pid=0

cleanup() {
  if [ "$pid" -gt 1 ]; then
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

make -B all >/dev/null
./build/libreecho-web \
  --backend mock \
  --config "$CFG" \
  --mock-config ./config/mock-state.json \
  --web-root ./web \
  --listen "127.0.0.1:$PORT" \
  --seed 42 \
  --dev-controls >"$LOG" 2>&1 &
pid=$!

i=0
while ! curl -fsS "$URL/api/v1/config" >"$TMP/api-config.json" 2>/dev/null; do
  i=$((i + 1))
  if [ "$i" -ge 50 ]; then
    cat "$LOG" >&2
    exit 1
  fi
  sleep 0.1
done
CSRF=$(jq -r '.data.csrf_token' "$TMP/api-config.json")

curl -fsS -X POST "$URL/api/v1/dev/mock" \
  -H "X-LibreEcho-CSRF: $CSRF" \
  -H 'Content-Type: application/json' \
  --data '{"action":"set-wake-available","value":"false"}' \
  >"$TMP/control.json"
jq -e '.ok == true and .data.applied == true' "$TMP/control.json" >/dev/null

curl -fsS -X POST "$URL/api/v1/setup" \
  -H "X-LibreEcho-CSRF: $CSRF" \
  -H 'Content-Type: application/json' \
  --data '{"hostname":"optional-adapter-test","ssid":"Open Test Network","security":"open","password":"","volume":52,"wake_word":"CustomWake","wake_sensitivity":77,"local_only":true,"diagnostic_telemetry":false}' \
  >"$TMP/setup.json"
jq -e '.ok == true and .data.completed == true' "$TMP/setup.json" >/dev/null
jq -e '.wake_word == "CustomWake" and .wake_sensitivity == 77' "$CFG" >/dev/null

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
pid=0

PORT=$((PORT + 1))
URL="http://127.0.0.1:$PORT"
CFG="$TMP/audio-config.json"
./build/libreecho-web \
  --backend mock \
  --config "$CFG" \
  --mock-config ./config/mock-state.json \
  --web-root ./web \
  --listen "127.0.0.1:$PORT" \
  --seed 43 \
  --dev-controls >"$LOG" 2>&1 &
pid=$!
i=0
while ! curl -fsS "$URL/api/v1/config" >"$TMP/api-config.json" 2>/dev/null; do
  i=$((i + 1))
  [ "$i" -lt 50 ] || { cat "$LOG" >&2; exit 1; }
  sleep 0.1
done
CSRF=$(jq -r '.data.csrf_token' "$TMP/api-config.json")
curl -fsS -X POST "$URL/api/v1/dev/mock" \
  -H "X-LibreEcho-CSRF: $CSRF" -H 'Content-Type: application/json' \
  --data '{"action":"fail-next","value":"audio"}' >/dev/null
curl -sS -X POST "$URL/api/v1/setup" \
  -H "X-LibreEcho-CSRF: $CSRF" -H 'Content-Type: application/json' \
  --data '{"hostname":"audio-stage-test","ssid":"Open Test Network","security":"open","password":"","volume":52,"wake_word":"LibreEcho","wake_sensitivity":70,"local_only":true,"diagnostic_telemetry":false}' \
  >"$TMP/audio-failure.json"
jq -e '.ok == false and (.error.message | test("audio"; "i"))' \
  "$TMP/audio-failure.json" >/dev/null

echo 'setup optional-adapter behavior and staged errors: ok'
