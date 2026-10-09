#!/bin/sh
# Active-device ping (telemetry Tier 0) disclosure contract.
#
# Starts the real web daemon in mock mode against fixture files laid out the
# way Platform's libreecho-ping writes them, and checks that GET /api/v1/privacy
# reports the ping as always on, carries the exact next payload verbatim, and
# never relays a tampered payload file as JSON.  Also pins the setup and
# Privacy page copy and the setup defaults.
set -eu
cd "$(dirname "$0")/.."
make -s build/libreecho-web >/dev/null
PORT=${LIBREECHO_PING_TEST_PORT:-18093}
URL="http://127.0.0.1:$PORT"
WORK=$(mktemp -d "${TMPDIR:-$PWD/build}/le-ping-XXXXXX")
pid=0
cleanup(){ [ "$pid" -gt 1 ] && kill "$pid" 2>/dev/null && wait "$pid" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

PAYLOAD='{"v":1,"hw":"radar","ver":"0.14.0","ch":"dev","build":"cc238ba","wk":"2026-W41","mo":"2026-10","yr":"2026","w":0,"m":0,"y":0}'
mkdir -p "$WORK/state"
printf '%s\n' "$PAYLOAD" > "$WORK/state/ping-next-payload"
printf 'schema=1\nresult=sent\nhttp=204\nperiod=2026-W41\npayload=%s\n' "$PAYLOAD" > "$WORK/state/ping-status"

start(){
    LIBREECHO_PING_STATE="$WORK/state" ./build/libreecho-web --backend mock --config "$WORK/config.json" \
        --web-root ./web --listen "127.0.0.1:$PORT" --seed 42 --dev-controls >"$WORK/server.log" 2>&1 &
    pid=$!
    i=0
    until curl -fsS "$URL/api/v1/config" >/dev/null 2>&1; do
        i=$((i + 1)); [ "$i" -lt 100 ] || { cat "$WORK/server.log"; exit 1; }; sleep 0.1
    done
}
stop(){ kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null || true; pid=0; }

start
curl -fsS "$URL/api/v1/privacy" > "$WORK/privacy.json"
jq -e --argjson want "$PAYLOAD" '
    .ok and
    .data.active_device_ping.enabled == true and
    .data.active_device_ping.can_disable == false and
    .data.active_device_ping.ip_stored == false and
    .data.active_device_ping.endpoint == "https://stats.libreecho.org/v1/ping" and
    .data.active_device_ping.user_agent == "libreecho-ping/1" and
    .data.active_device_ping.next_payload == $want and
    .data.active_device_ping.last == {"result":"sent","http":"204","period":"2026-W41"} and
    ([.data.active_device_ping.fields[].name] == ["v","hw","ver","ch","build","wk","mo","yr","w","m","y"]) and
    (.data | has("diagnostic_telemetry") and has("crash_reports"))' "$WORK/privacy.json" >/dev/null

# The ping block is read-only: PUT cannot switch it off.
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
curl -fsS -X PUT "$URL/api/v1/privacy" -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"active_device_ping":{"enabled":false},"diagnostic_telemetry":false,"crash_reports":false}' >/dev/null
curl -fsS "$URL/api/v1/privacy" | jq -e '
    .data.active_device_ping.enabled == true and
    .data.diagnostic_telemetry == false and .data.crash_reports == false' >/dev/null
stop

# A tampered or missing payload file is reported as null, never injected.
printf '%s\n' '{"v":1,"x":"</script><b>"}' > "$WORK/state/ping-next-payload"
printf 'schema=1\nresult=evil"\nhttp=2x\nperiod=<b>\n' > "$WORK/state/ping-status"
start
curl -fsS "$URL/api/v1/privacy" | jq -e '
    .data.active_device_ping.next_payload == null and .data.active_device_ping.last == null' >/dev/null
stop
rm -rf "$WORK/state"
start
curl -fsS "$URL/api/v1/privacy" | jq -e '
    .data.active_device_ping.enabled == true and
    .data.active_device_ping.next_payload == null and .data.active_device_ping.last == null' >/dev/null
stop

# Setup accepts crash_reports, and older clients that omit it still work.
start
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/setup" -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"hostname":"ping-echo","ssid":"Open","security":"open","password":"","volume":40,"wake_word":"LibreEcho","wake_sensitivity":60,"local_only":true,"diagnostic_telemetry":true,"crash_reports":"yes"}')
[ "$code" = 400 ]
curl -fsS -X POST "$URL/api/v1/setup" -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"hostname":"ping-echo","ssid":"Open","security":"open","password":"","volume":40,"wake_word":"LibreEcho","wake_sensitivity":60,"local_only":true,"diagnostic_telemetry":true,"crash_reports":true}' |
    jq -e '.ok and .data.completed' >/dev/null
curl -fsS "$URL/api/v1/privacy" | jq -e '.data.diagnostic_telemetry == true and .data.crash_reports == true' >/dev/null
stop

# Copy and defaults: the ping is disclosed and the two opt-outs start ticked.
grep -q 'Once a week LibreEcho sends one anonymous ping to stats.libreecho.org' web/setup.html
grep -q 'This ping cannot be turned off' web/setup.html
grep -q '<input id="setup-telemetry" type="checkbox" checked>' web/setup.html
grep -q '<input id="setup-crash-reports" type="checkbox" checked>' web/setup.html
grep -q 'diagnostic_telemetry:true,crash_reports:true}}' web/js/setup.js
grep -q 'This ping cannot be turned off' web/js/privacy-ui.js
grep -q 'activePingPanel(p.active_device_ping)' web/js/privacy-ui.js
! grep -q 'No cloud dependency is enabled by default' web/js/app.js web/js/privacy-ui.js
echo "active-device ping disclosure: ok"
