#!/bin/sh
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CFG=${LIBREECHO_TEST_CONFIG:-./build/test-suite-config.json}
CSRF='X-LibreEcho-CSRF: libreecho-local'
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
expect "$(curl -fsS "$URL/setup.html")" 'Connect LibreEcho to Wi-Fi'
expect "$(curl -fsS "$URL/js/setup.js")" "api('/setup'"
expect "$(curl -fsS "$URL/api/v1/device")" '"serial":"DEV-MOCK'
expect "$(curl -fsS "$URL/api/v1/network/wifi/scan")" 'LibreNet-5G'
code=$(curl -sS -o /tmp/le-invalid.out -w '%{http_code}' -X PUT "$URL/api/v1/audio" -H "$CSRF" -H 'Content-Type: application/json' --data '{bad')
[ "$code" = 400 ]
code=$(curl -sS -o /tmp/le-csrf.out -w '%{http_code}' -X PUT "$URL/api/v1/audio" -H 'Content-Type: application/json' --data '{"volume":20}')
[ "$code" = 403 ]
code=$(curl -sS -o /tmp/le-confirm.out -w '%{http_code}' -X POST "$URL/api/v1/system/reboot" -H "$CSRF" -H 'Content-Type: application/json' --data '{}')
[ "$code" = 403 ]
echo 'api: ok'
