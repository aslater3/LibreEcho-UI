#!/bin/sh
# The Bluetooth page switch (PUT /api/v1/bluetooth {"enabled":...}) must survive
# a restart. It used to power the radio without touching the saved integration
# bit or writing the config, so the next boot came up with Bluetooth off.
set -eu

PORT=${LIBREECHO_BT_PERSIST_PORT:-$((19000 + ($$ % 10000)))}
URL="http://127.0.0.1:$PORT"
TMP=$(mktemp -d)
CFG="$TMP/web-config.json"
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

start_server() {
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
    [ "$i" -lt 50 ] || { cat "$LOG" >&2; exit 1; }
    sleep 0.1
  done
  CSRF=$(jq -r '.data.csrf_token' "$TMP/api-config.json")
}

restart_server() {
  kill "$pid"
  wait "$pid" 2>/dev/null || true
  pid=0
  PORT=$((PORT + 1))
  URL="http://127.0.0.1:$PORT"
  start_server
}

set_bluetooth() {
  curl -fsS -X PUT "$URL/api/v1/bluetooth" \
    -H "X-LibreEcho-CSRF: $CSRF" \
    -H 'Content-Type: application/json' \
    --data "{\"enabled\":$1}" >"$TMP/put.json"
  jq -e ".ok == true and .data.enabled == $1" "$TMP/put.json" >/dev/null
}

expect_after_restart() {
  curl -fsS "$URL/api/v1/bluetooth" >"$TMP/bt.json"
  jq -e ".data.enabled == $1" "$TMP/bt.json" >/dev/null || {
    echo "bluetooth enabled != $1 after restart" >&2; cat "$TMP/bt.json" >&2; exit 1; }
  curl -fsS "$URL/api/v1/integrations" >"$TMP/integrations.json"
  jq -e ".data.items[] | select(.id == \"bluetooth\") | .enabled == $1" \
    "$TMP/integrations.json" >/dev/null
}

start_server

set_bluetooth true
jq -e '(.integrations / 8 | floor) % 2 == 1' "$CFG" >/dev/null
restart_server
expect_after_restart true

set_bluetooth false
jq -e '(.integrations / 8 | floor) % 2 == 0' "$CFG" >/dev/null
restart_server
expect_after_restart false

# discoverable/connectable go through the same PUT but must not flip the bit.
set_bluetooth true
curl -fsS -X PUT "$URL/api/v1/bluetooth" \
  -H "X-LibreEcho-CSRF: $CSRF" \
  -H 'Content-Type: application/json' \
  --data '{"discoverable":false}' >/dev/null
jq -e '(.integrations / 8 | floor) % 2 == 1' "$CFG" >/dev/null

printf '%s\n' 'Bluetooth page enable/disable persists across restart: ok'
