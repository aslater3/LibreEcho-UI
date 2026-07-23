#!/bin/sh
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CFG=${LIBREECHO_TEST_CONFIG:-./build/test-suite-config.json}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
expect(){ printf '%s' "$1" | grep -q "$2" || { echo "expected $2 in $1" >&2; exit 1; }; }
expect "$(curl -fsS "$URL/api/v1/status")" '"backend":"mock"'
expect "$(curl -fsS "$URL/api/v1")" '"swagger":"/swagger.html"'
expect "$(curl -fsS "$URL/api/v1/setup")" '"completed":false'
expect "$(curl -fsS "$URL/")" 'First-boot setup'
code=$(curl -sS -o /tmp/le-bad-setup.out -w '%{http_code}' -X POST "$URL/api/v1/setup" -H "$CSRF" -H 'Content-Type: application/json' --data '{"hostname":"bad host"}')
[ "$code" = 400 ]
setup='{"hostname":"kitchen-echo","ssid":"LibreNet-IoT","security":"wpa2","password":"top-secret","volume":52,"wake_word":"LibreEcho","wake_sensitivity":72,"local_only":true,"diagnostic_telemetry":false}'
expect "$(curl -fsS -X POST "$URL/api/v1/setup" -H "$CSRF" -H 'Content-Type: application/json' --data "$setup")" '"completed":true'
expect "$(curl -fsS "$URL/api/v1/config")" '"setup_completed":true'
expect "$(curl -fsS "$URL/")" 'LibreEcho Control Centre'
code=$(curl -sS -o /tmp/le-repeat-setup.out -w '%{http_code}' -X POST "$URL/api/v1/setup" -H "$CSRF" -H 'Content-Type: application/json' --data "$setup")
[ "$code" = 409 ]
! grep -q 'top-secret' "$CFG"
curl -fsS "$URL/openapi.json" | grep -Eq '"openapi"[[:space:]]*:[[:space:]]*"3.0.3"'
expect "$(curl -fsS "$URL/swagger.html")" 'LibreEcho API Reference'
expect "$(curl -fsS "$URL/js/swagger.js")" 'executeOperation'
expect "$(curl -fsS "$URL/api/v1/device")" '"serial":"DEV-MOCK'
curl -fsS "$URL/api/v1/playback" | jq -e \
    '.ok and .data.state == "playing" and
     .data.source == "airplay2" and .data.buses.media == true and
     .data.metadata.available == true and
     .data.metadata.title == "Open Source Radio"' >/dev/null
curl -fsS "$URL/api/v1/led" | jq -e \
    '.ok and (.data.visualizer_enabled == true) and
     (.data.visualizer_active == false) and
     (.data.visualizer_owner == "") and
     (.data.visualizer_mood == "idle") and
     (.data.visualizer_levels | length == 12) and
     (.data.pixels | length == 12) and
     all(.data.pixels[]; (.r >= 0 and .r <= 255) and
                         (.g >= 0 and .g <= 255) and
                         (.b >= 0 and .b <= 255))' >/dev/null
curl -fsS -X PUT "$URL/api/v1/led" -H "$CSRF" \
    -H 'Content-Type: application/json' \
    --data '{"visualizer_enabled":false}' |
    jq -e '.ok and .data.visualizer_enabled == false and
           .data.visualizer_active == false' >/dev/null
curl -fsS -X PUT "$URL/api/v1/led" -H "$CSRF" \
    -H 'Content-Type: application/json' \
    --data '{"visualizer_enabled":true}' |
    jq -e '.ok and .data.visualizer_enabled == true' >/dev/null
code=$(curl -sS -o /tmp/le-invalid-visualizer.out -w '%{http_code}' \
    -X PUT "$URL/api/v1/led" -H "$CSRF" \
    -H 'Content-Type: application/json' \
    --data '{"visualizer_enabled":"yes"}')
[ "$code" = 400 ]
expect "$(curl -fsS "$URL/api/v1/baby-monitor")" '"sources":'
code=$(curl -sS -o /tmp/le-baby-stream.out -w '%{http_code}' "$URL/api/v1/baby-monitor/stream?source=0:0")
[ "$code" = 501 ]expect "$(curl -fsS "$URL/api/v1/bluetooth")" '"capabilities":'
curl -fsS -X PUT "$URL/api/v1/bluetooth" -H "$CSRF" -H 'Content-Type: application/json' --data '{"enabled":true}' | jq -e '.ok and .data.enabled == true' >/dev/null
curl -fsS -X PUT "$URL/api/v1/bluetooth" -H "$CSRF" -H 'Content-Type: application/json' --data '{"connectable":false}' | jq -e '.ok and .data.capabilities.connectable == false' >/dev/null
curl -fsS -X PUT "$URL/api/v1/bluetooth" -H "$CSRF" -H 'Content-Type: application/json' --data '{"discoverable":true}' | jq -e '.ok and .data.capabilities.discoverable == true' >/dev/null
curl -fsS -X POST "$URL/api/v1/bluetooth/scan" -H "$CSRF" -H 'Content-Type: application/json' --data '{}' | jq -e '.ok and .data.scanning == true' >/dev/null
curl -fsS -X POST "$URL/api/v1/bluetooth/pair" -H "$CSRF" -H 'Content-Type: application/json' --data '{"address":"10:20:30:40:50:60","type":0,"io_capability":3}' | jq -e '.ok and (.data.known_devices | length) == 1' >/dev/null
curl -fsS -X POST "$URL/api/v1/bluetooth/unpair" -H "$CSRF" -H 'Content-Type: application/json' --data '{"address":"10:20:30:40:50:60","type":0}' | jq -e '.ok and (.data.known_devices | length) == 0' >/dev/null
expect "$(curl -fsS "$URL/api/v1/network/wifi/scan")" 'LibreNet-5G'
expect "$(curl -fsS "$URL/api/v1/system")" '"ntp":false'
expect "$(curl -fsS "$URL/api/v1/system")" '"clock_valid":true'
expect "$(curl -fsS "$URL/api/v1/logs")" '"boot_seconds":'
curl -fsS -D /tmp/le-log-stream.headers "$URL/api/v1/logs/stream" -o /tmp/le-log-stream.out
grep -qi '^content-type: text/event-stream' /tmp/le-log-stream.headers
grep -q '^event: logs$' /tmp/le-log-stream.out
grep -q '^data: {"ok":true' /tmp/le-log-stream.out
code=$(curl -sS -o /tmp/le-invalid.out -w '%{http_code}' -X PUT "$URL/api/v1/audio" -H "$CSRF" -H 'Content-Type: application/json' --data '{bad')
[ "$code" = 400 ]
code=$(curl -sS -o /tmp/le-csrf.out -w '%{http_code}' -X PUT "$URL/api/v1/audio" -H 'Content-Type: application/json' --data '{"volume":20}')
[ "$code" = 403 ]
code=$(curl -sS -o /tmp/le-confirm.out -w '%{http_code}' -X POST "$URL/api/v1/system/reboot" -H "$CSRF" -H 'Content-Type: application/json' --data '{}')
[ "$code" = 403 ]
code=$(curl -sS -o /tmp/le-method.out -w '%{http_code}' -X DELETE "$URL/api/v1/buttons" -H "$CSRF")
[ "$code" = 405 ]
code=$(curl -sS -o /tmp/le-method.out -w '%{http_code}' "$URL/api/v1/integrations/home-assistant")
[ "$code" = 405 ]
escaped=$(curl -fsS -X PUT "$URL/api/v1/buttons" -H "$CSRF" -H 'Content-Type: application/json' --data '{"short_press":"say \"hi\"","long_press":"path\\test"}')
printf '%s' "$escaped" | jq -e '.ok and .data.short_press == "say \"hi\"" and .data.long_press == "path\\test"' >/dev/null
echo 'api: ok'
