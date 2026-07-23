#!/bin/sh
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
expect(){ printf '%s' "$1" | grep -q "$2" || { echo "expected $2 in $1" >&2; exit 1; }; }
expect "$(curl -fsS "$URL/api/v1/status")" '"backend":"mock"'
expect "$(curl -fsS "$URL/api/v1")" '"swagger":"/swagger.html"'
curl -fsS "$URL/openapi.json" | grep -Eq '"openapi"[[:space:]]*:[[:space:]]*"3.0.3"'
expect "$(curl -fsS "$URL/swagger.html")" 'LibreEcho API Reference'
expect "$(curl -fsS "$URL/js/swagger.js")" 'executeOperation'
expect "$(curl -fsS "$URL/api/v1/device")" '"serial":"DEV-MOCK'
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
