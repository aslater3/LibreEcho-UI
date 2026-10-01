#!/bin/sh
# Focused HTTP contract test for the feature-batch daemon APIs.
#
# Starts its own mock-backend web daemon plus a deterministic agentd stand-in
# (tests/test_feature_batch_api_agent.py) on isolated ports/sockets, then drives
# the frozen endpoints over real HTTP: strict validation, auth/CSRF denial,
# capability gating, recovery secret handling and the voice-history routes.
#
# Usage: sh tests/test_feature_batch_api.sh
# Requires: build/libreecho-web (make build/libreecho-web), jq, curl.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=${LIBREECHO_FEATURE_TEST_PORT:-18099}
AUTH_PORT=${LIBREECHO_FEATURE_TEST_AUTH_PORT:-18098}
URL="http://127.0.0.1:$PORT"
AUTH_URL="http://127.0.0.1:$AUTH_PORT"
BIN="$ROOT/build/libreecho-web"
TMP=$(mktemp -d)
PASS=0
FAIL=0

cleanup() {
    [ -n "${dev_pid:-}" ] && kill "$dev_pid" 2>/dev/null
    [ -n "${auth_pid:-}" ] && kill "$auth_pid" 2>/dev/null
    [ -n "${agent_pid:-}" ] && kill "$agent_pid" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

[ -x "$BIN" ] || { echo "missing $BIN; run: make build/libreecho-web" >&2; exit 1; }

ok() { PASS=$((PASS + 1)); }
bad() { FAIL=$((FAIL + 1)); echo "FAIL[$1] $2" >&2; [ -s "$TMP/body" ] && { echo "  body: $(cat "$TMP/body")" >&2; }; }

# check <desc> <want-status> <curl args...>
check() {
    desc=$1; want=$2; shift 2
    got=$(curl -sS -o "$TMP/body" -w '%{http_code}' "$@") || got=000
    if [ "$got" = "$want" ]; then ok; else bad "$desc" "want $want got $got"; fi
}
# jcheck <desc> <jq-expr> <curl args...>
jcheck() {
    desc=$1; expr=$2; shift 2
    if ! curl -fsS -o "$TMP/body" "$@"; then bad "$desc" "request failed"; return; fi
    if out=$(jq -e "$expr" "$TMP/body" 2>&1); then ok; else bad "$desc" "jq($?) $expr :: $out"; fi
}

echo "== starting fixtures =="
CFG="$TMP/config.json"
printf '{}' > "$CFG"
export LIBREECHO_AGENT_SOCKET="$TMP/agent.sock"
python3 "$ROOT/tests/test_feature_batch_api_agent.py" "$LIBREECHO_AGENT_SOCKET" >"$TMP/agent.log" 2>&1 &
agent_pid=$!
i=0
while [ ! -S "$LIBREECHO_AGENT_SOCKET" ]; do
    i=$((i + 1)); [ "$i" -lt 50 ] || { echo "agent fixture did not start" >&2; cat "$TMP/agent.log" >&2; exit 1; }
    sleep 0.1
done

"$BIN" --backend mock --config "$CFG" --mock-config "$ROOT/config/mock-state.json" \
    --web-root "$ROOT/web" --listen "127.0.0.1:$PORT" --seed 42 --dev-controls \
    >"$TMP/dev.log" 2>&1 &
dev_pid=$!
i=0
while ! curl -fsS "$URL/api/v1/status" >/dev/null 2>&1; do
    i=$((i + 1)); [ "$i" -lt 150 ] || { echo "dev server did not start" >&2; cat "$TMP/dev.log" >&2; exit 1; }
    sleep 0.1
done

CSRF=$(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')
[ -n "$CSRF" ] && [ "$CSRF" != null ] || { echo "no CSRF token" >&2; exit 1; }
AUTH="X-LibreEcho-CSRF: $CSRF"
JSON='Content-Type: application/json'

echo "== auth/CSRF denial =="
check "led/idle no CSRF" 403 -X PUT "$URL/api/v1/led/idle" -H "$JSON" --data '{"mode":"always"}'
check "noise no CSRF" 403 -X POST "$URL/api/v1/audio/noise" -H "$JSON" --data '{"level":10}'
check "recovery no CSRF" 403 -X POST "$URL/api/v1/network/recovery/prepare" -H "$JSON" --data ''
check "recovery PUT no CSRF" 403 -X PUT "$URL/api/v1/network/recovery" -H "$JSON" --data '{"enabled":true,"auto_enabled":false,"timeout_seconds":60}'
check "led/idle GET method" 405 -X GET "$URL/api/v1/led/idle"
check "led/sleep POST method" 405 -X POST "$URL/api/v1/led/sleep" -H "$AUTH" -H "$JSON" --data '{"mode":"solid"}'
check "history POST method" 405 -X POST "$URL/api/v1/assistant/history" -H "$AUTH" -H "$JSON" --data '{}'
check "history entry DELETE method" 405 -X DELETE "$URL/api/v1/assistant/history/1" -H "$AUTH" -H "$JSON" --data '{}'
check "latency POST method" 405 -X POST "$URL/api/v1/assistant/latency" -H "$AUTH" -H "$JSON" --data '{}'
check "network/recovery GET method" 405 -X GET "$URL/api/v1/network/recovery"

echo "== strict validation: led =="
check "led/idle bad mode" 400 -X PUT "$URL/api/v1/led/idle" -H "$AUTH" -H "$JSON" --data '{"mode":"bogus"}'
check "led/idle missing mode" 400 -X PUT "$URL/api/v1/led/idle" -H "$AUTH" -H "$JSON" --data '{}'
check "led/sleep bad mode" 400 -X PUT "$URL/api/v1/led/sleep" -H "$AUTH" -H "$JSON" --data '{"mode":"blink"}'
check "led/sleep brightness range" 400 -X PUT "$URL/api/v1/led/sleep" -H "$AUTH" -H "$JSON" --data '{"mode":"solid","brightness":21}'
check "led/sleep period range" 400 -X PUT "$URL/api/v1/led/sleep" -H "$AUTH" -H "$JSON" --data '{"mode":"solid","period_ms":1000}'
check "led/sleep timer range" 400 -X PUT "$URL/api/v1/led/sleep" -H "$AUTH" -H "$JSON" --data '{"mode":"solid","timer_minutes":721}'
check "led/sleep malformed type" 400 -X PUT "$URL/api/v1/led/sleep" -H "$AUTH" -H "$JSON" --data '{"mode":"solid","timer_minutes":"5"}'
check "led/sleep bad bool" 400 -X PUT "$URL/api/v1/led/sleep" -H "$AUTH" -H "$JSON" --data '{"mode":"solid","restore_on_boot":"yes"}'
check "led/sleep duplicate key" 400 -X PUT "$URL/api/v1/led/sleep" -H "$AUTH" -H "$JSON" --data '{"mode":"solid","mode":"pulse"}'
check "led/sleep malformed json" 400 -X PUT "$URL/api/v1/led/sleep" -H "$AUTH" -H "$JSON" --data '{'

echo "== strict validation: noise =="
check "noise bad source" 400 -X POST "$URL/api/v1/audio/noise" -H "$AUTH" -H "$JSON" --data '{"source":"green"}'
check "noise legacy bad colour" 400 -X POST "$URL/api/v1/audio/noise" -H "$AUTH" -H "$JSON" --data '{"colour":"magenta"}'
check "noise level range" 400 -X POST "$URL/api/v1/audio/noise" -H "$AUTH" -H "$JSON" --data '{"level":0}'
check "noise minutes range" 400 -X POST "$URL/api/v1/audio/noise" -H "$AUTH" -H "$JSON" --data '{"minutes":601}'
check "noise bad bed" 400 -X POST "$URL/api/v1/audio/noise" -H "$AUTH" -H "$JSON" --data '{"bed":"green"}'
check "noise tempo range" 400 -X POST "$URL/api/v1/audio/noise" -H "$AUTH" -H "$JSON" --data '{"source":"heartbeat","tempo":39}'
check "noise fade range" 400 -X POST "$URL/api/v1/audio/noise" -H "$AUTH" -H "$JSON" --data '{"source":"heartbeat","fade_seconds":601}'
check "noise fade over timer" 400 -X POST "$URL/api/v1/audio/noise" -H "$AUTH" -H "$JSON" --data '{"minutes":1,"fade_seconds":120}'
check "noise duplicate key" 400 -X POST "$URL/api/v1/audio/noise" -H "$AUTH" -H "$JSON" --data '{"level":10,"level":20}'
check "noise malformed json" 400 -X POST "$URL/api/v1/audio/noise" -H "$AUTH" -H "$JSON" --data 'not json'

echo "== strict validation: recovery =="
check "recovery missing fields" 400 -X PUT "$URL/api/v1/network/recovery" -H "$AUTH" -H "$JSON" --data '{}'
check "recovery bad bool" 400 -X PUT "$URL/api/v1/network/recovery" -H "$AUTH" -H "$JSON" --data '{"enabled":"yes","auto_enabled":true,"timeout_seconds":60}'
check "recovery timeout low" 400 -X PUT "$URL/api/v1/network/recovery" -H "$AUTH" -H "$JSON" --data '{"enabled":true,"auto_enabled":true,"timeout_seconds":10}'
check "recovery timeout high" 400 -X PUT "$URL/api/v1/network/recovery" -H "$AUTH" -H "$JSON" --data '{"enabled":true,"auto_enabled":true,"timeout_seconds":601}'
check "recovery duplicate key" 400 -X PUT "$URL/api/v1/network/recovery" -H "$AUTH" -H "$JSON" --data '{"enabled":true,"enabled":false,"auto_enabled":true,"timeout_seconds":60}'

echo "== capability gating: usb playback =="
check "opus play refused" 415 -X POST "$URL/api/v1/storage/usb/play" -H "$AUTH" -H "$JSON" --data '{"path":"music/song.opus"}'
check "aac play refused" 415 -X POST "$URL/api/v1/storage/usb/play" -H "$AUTH" -H "$JSON" --data '{"path":"music/song.aac"}'
check "play path escape" 400 -X POST "$URL/api/v1/storage/usb/play" -H "$AUTH" -H "$JSON" --data '{"path":"../etc/passwd"}'
check "play path required" 400 -X POST "$URL/api/v1/storage/usb/play" -H "$AUTH" -H "$JSON" --data '{}'
jcheck "playable_formats default mp3" '.data.playable_formats == ["mp3"]' "$URL/api/v1/storage/usb"

echo "== success: led =="
check "led/idle set" 200 -X PUT "$URL/api/v1/led/idle" -H "$AUTH" -H "$JSON" --data '{"mode":"indicator"}'
jcheck "led idle_mode reflected" '.data.idle_mode == "indicator"' "$URL/api/v1/led"
check "led/sleep set" 200 -X PUT "$URL/api/v1/led/sleep" -H "$AUTH" -H "$JSON" --data '{"mode":"pulse","brightness":6,"period_ms":6000,"timer_minutes":30,"restore_on_boot":true}'
jcheck "led sleep_light reflected" '.data.sleep_light.mode == "pulse" and .data.sleep_light.brightness == 6 and .data.sleep_light.period_ms == 6000 and .data.sleep_light.timer_minutes == 30 and .data.sleep_light.restore_on_boot == true' "$URL/api/v1/led"
jcheck "led existing controls preserved" '.data.visualizer_enabled == true and (.data.brightness | type == "number") and (.data.night.enabled | type == "boolean")' "$URL/api/v1/led"

echo "== success: noise =="
check "noise extended" 200 -X POST "$URL/api/v1/audio/noise" -H "$AUTH" -H "$JSON" --data '{"source":"brown","bed":"pink","level":35,"minutes":10,"tempo":60,"fade_seconds":30}'
jcheck "noise fields reflected" '.data.noise.source == "brown" and .data.noise.bed == "pink" and .data.noise.level == 35 and .data.noise.tempo == 60 and .data.noise.fade_seconds == 30' "$URL/api/v1/audio"
check "noise legacy colour" 200 -X POST "$URL/api/v1/audio/noise" -H "$AUTH" -H "$JSON" --data '{"colour":"pink","level":40,"minutes":0}'
jcheck "legacy colour maps to source" '.data.noise.source == "pink" and .data.noise.colour == "pink"' "$URL/api/v1/audio"
check "noise stop" 200 -X DELETE "$URL/api/v1/audio/noise" -H "$AUTH" -H "$JSON" --data '{}'

echo "== success: recovery =="
check "recovery configure" 200 -X PUT "$URL/api/v1/network/recovery" -H "$AUTH" -H "$JSON" --data '{"enabled":true,"auto_enabled":false,"timeout_seconds":120}'
jcheck "recovery reflected" '.data.recovery.enabled == true and .data.recovery.auto_enabled == false and .data.recovery.auto_timeout_ms == 120000' "$URL/api/v1/network"
jcheck "GET network has no psk/psk_path" '[paths | select(.[-1] == "psk" or .[-1] == "psk_path")] | length == 0' "$URL/api/v1/network"
check "recovery prepare" 200 -X POST "$URL/api/v1/network/recovery/prepare" -H "$AUTH" -H "$JSON" --data ''
jcheck "prepare returns ssid+psk" '.data.psk | type == "string" and length > 0' "$URL/api/v1/network/recovery/prepare" -X POST -H "$AUTH" -H "$JSON" --data ''
# Header check: the secret must never be cached by a proxy or the browser.
curl -sS -D "$TMP/hdr" -o "$TMP/body" -X POST "$URL/api/v1/network/recovery/prepare" -H "$AUTH" -H "$JSON" --data ''
if grep -qi '^cache-control:.*no-store' "$TMP/hdr"; then ok; else bad "prepare no-store" "$(grep -i cache-control "$TMP/hdr" || echo 'no Cache-Control')"; fi
check "recovery stop" 200 -X POST "$URL/api/v1/network/recovery/stop" -H "$AUTH" -H "$JSON" --data '{}'
jcheck "recovery stop reflected" '.data.recovery.mode == "off"' "$URL/api/v1/network"

echo "== success: voice history =="
jcheck "history collection" '.data.capacity == 10 and .data.history_generation == 7 and (.data.turns | length) == 1' "$URL/api/v1/assistant/history"
jcheck "history escaping round-trip" '.data.turns[0].transcript_preview == "turn \"quoted\" \\ backslash caf\u00e9"' "$URL/api/v1/assistant/history"
jcheck "history entry detail" '.data.id == 4242 and (.data.transcript | type == "string")' "$URL/api/v1/assistant/history/4242"
check "history entry unknown id" 404 -X GET "$URL/api/v1/assistant/history/999"
check "history entry non-numeric" 404 -X GET "$URL/api/v1/assistant/history/abc"
check "history entry zero" 404 -X GET "$URL/api/v1/assistant/history/0"
check "history clear delete" 200 -X DELETE "$URL/api/v1/assistant/history" -H "$AUTH" -H "$JSON" --data '{}'
check "history clear post" 200 -X POST "$URL/api/v1/assistant/history/clear" -H "$AUTH" -H "$JSON" --data '{}'
jcheck "latency alias old shape" '.data.history_generation == 3 and .data.turns[0].first_pcm_ms == 3100' "$URL/api/v1/assistant/latency"

echo "== auth enforcement (token server) =="
printf 'feature-token-0123456789abcdef' > "$TMP/token"
chmod 600 "$TMP/token"
"$BIN" --backend mock --config "$TMP/authconfig.json" --mock-config "$ROOT/config/mock-state.json" \
    --web-root "$ROOT/web" --listen "127.0.0.1:$AUTH_PORT" --seed 7 --dev-controls \
    --auth-token-file "$TMP/token" >"$TMP/auth.log" 2>&1 &
auth_pid=$!
i=0
while [ ! -s "$TMP/auth.log" ] && [ "$i" -lt 50 ]; do i=$((i + 1)); sleep 0.1; done
i=0
while [ "$(curl -sS -o /dev/null -w '%{http_code}' "$AUTH_URL/api/v1/status" 2>/dev/null)" != "401" ]; do
    i=$((i + 1)); [ "$i" -lt 150 ] || { echo "auth server did not start" >&2; cat "$TMP/auth.log" >&2; exit 1; }
    sleep 0.1
done
check "auth: GET network denied" 401 "$AUTH_URL/api/v1/network"
check "auth: recovery prepare denied" 401 -X POST "$AUTH_URL/api/v1/network/recovery/prepare" -H "$JSON" --data ''
check "auth: recovery stop denied" 401 -X POST "$AUTH_URL/api/v1/network/recovery/stop" -H "$JSON" --data '{}'
check "auth: GET network allowed" 200 -H 'Authorization: Bearer feature-token-0123456789abcdef' "$AUTH_URL/api/v1/network"

echo
echo "feature-batch API: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
