#!/bin/sh
set -eu

PORT=${LIBREECHO_AIRPLAY_SETUP_PORT:-$((18000 + ($$ % 10000)))}
URL="http://127.0.0.1:$PORT"
TMP=$(mktemp -d)
CFG="$TMP/web-config.json"
LOG="$TMP/server.log"
ACTIVATOR_MARKER="$TMP/activator-called"
READY="$TMP/startup-ready"
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
printf '%s\n' '#!/bin/sh' '[ "$#" -eq 0 ] || exit 2' "printf 'schema=1\\n' > '$READY'; touch '$ACTIVATOR_MARKER'" >"$TMP/activator.sh"
chmod 755 "$TMP/activator.sh"
export LIBREECHO_SETUP_FEATURE_ACTIVATOR="$TMP/activator.sh"
export LIBREECHO_SETUP_STARTUP_READY="$READY"
export LIBREECHO_SETUP_READY_TIMEOUT_TICKS=3

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

start_server

curl -fsS -X POST "$URL/api/v1/setup" \
  -H "X-LibreEcho-CSRF: $CSRF" \
  -H 'Content-Type: application/json' \
  --data '{"hostname":"airplay-setup-test","ssid":"Open Test Network","security":"open","password":"","volume":52,"wake_word":"LibreEcho","wake_sensitivity":70,"local_only":true,"diagnostic_telemetry":false}' \
  >"$TMP/setup.json"
jq -e '.ok == true and .data.completed == true' "$TMP/setup.json" >/dev/null
jq -e '.integrations == 20 and .wake_word == "LibreEcho" and .wake_sensitivity == 70' "$CFG" >/dev/null
test -f "$ACTIVATOR_MARKER"
test -f "$READY"

curl -fsS -X PUT "$URL/api/v1/integrations/airplay2" \
  -H "X-LibreEcho-CSRF: $CSRF" \
  -H 'Content-Type: application/json' \
  --data '{"enabled":false}' \
  >"$TMP/disable.json"
jq -e '.ok == true and (.data.items[] | select(.id == "airplay2") | .enabled == false)' "$TMP/disable.json" >/dev/null
jq -e '.integrations == 4' "$CFG" >/dev/null

kill "$pid"
wait "$pid" 2>/dev/null || true
pid=0
PORT=$((PORT + 1))
URL="http://127.0.0.1:$PORT"
start_server
curl -fsS "$URL/api/v1/integrations" >"$TMP/restarted-integrations.json"
jq -e '.data.items[] | select(.id == "airplay2") | .enabled == false' \
  "$TMP/restarted-integrations.json" >/dev/null
kill "$pid"
wait "$pid" 2>/dev/null || true
pid=0

CFG="$TMP/airplay-failure-config.json"
PORT=$((PORT + 1))
URL="http://127.0.0.1:$PORT"
rm -f "$READY"
printf '%s\n' '#!/bin/sh' '[ "$#" -eq 0 ] || exit 2' "printf 'schema=1\\n' > '$READY'" >"$TMP/activator.sh"
start_server
curl -fsS -X POST "$URL/api/v1/dev/mock" \
  -H "X-LibreEcho-CSRF: $CSRF" \
  -H 'Content-Type: application/json' \
  --data '{"action":"fail-next","value":"airplay"}' >/dev/null
code=$(curl -sS -o "$TMP/setup-airplay-failure.json" -w '%{http_code}' \
  -X POST "$URL/api/v1/setup" \
  -H "X-LibreEcho-CSRF: $CSRF" \
  -H 'Content-Type: application/json' \
  --data '{"hostname":"airplay-health-failure","ssid":"Open Test Network","security":"open","password":"","volume":52,"wake_word":"LibreEcho","wake_sensitivity":70,"local_only":true,"diagnostic_telemetry":false}')
[ "$code" = 503 ]
jq -e '.ok == false and (.error.message | test("feature services"; "i"))' \
  "$TMP/setup-airplay-failure.json" >/dev/null
[ ! -e "$CFG.setup-complete" ]
kill "$pid"
wait "$pid" 2>/dev/null || true
pid=0

CFG="$TMP/failure-config.json"
PORT=$((PORT + 1))
URL="http://127.0.0.1:$PORT"
rm -f "$READY"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$TMP/activator.sh"
start_server
code=$(curl -sS -o "$TMP/setup-failure.json" -w '%{http_code}' \
  -X POST "$URL/api/v1/setup" \
  -H "X-LibreEcho-CSRF: $CSRF" \
  -H 'Content-Type: application/json' \
  --data '{"hostname":"airplay-setup-failure","ssid":"Open Test Network","security":"open","password":"","volume":52,"wake_word":"LibreEcho","wake_sensitivity":70,"local_only":true,"diagnostic_telemetry":false}')
[ "$code" = 503 ]
jq -e '.ok == false and (.error.message | test("feature services"; "i"))' \
  "$TMP/setup-failure.json" >/dev/null
[ ! -e "$CFG.setup-complete" ]

printf '%s\n' 'AirPlay setup default and explicit disable persistence: ok'
