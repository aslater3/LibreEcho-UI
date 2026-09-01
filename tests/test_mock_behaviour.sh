#!/bin/sh
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
ctl(){ curl -fsS -X POST "$URL/api/v1/dev/mock" -H "$CSRF" -H 'Content-Type: application/json' --data "{\"action\":\"$1\",\"value\":\"${2:-}\"}" >/dev/null; }
ctl set-temperature 72
curl -fsS "$URL/api/v1/status" | grep -Eq '"temperature_c":(69|70|71|72|73|74|75)'
ctl fail-next wifi-scan
code=$(curl -sS -o /tmp/le-fault.out -w '%{http_code}' "$URL/api/v1/network/wifi/scan")
[ "$code" = 501 ]
ctl trigger wake-word
curl -fsS "$URL/api/v1/wake-word" | grep -q '"detected_count":1'
curl -fsS -X POST "$URL/api/v1/network/wifi/connect" -H "$CSRF" -H 'Content-Type: application/json' --data '{"ssid":"LibreNet-IoT","password":"top-secret","security":"wpa2"}' >/dev/null
attempt=0
while ! curl -fsS "$URL/api/v1/network" | grep -q '"ssid":"LibreNet-IoT"'; do
    attempt=$((attempt + 1))
    [ "$attempt" -lt 30 ] || { echo "mock Wi-Fi association did not complete" >&2; exit 1; }
    sleep 0.2
done
! curl -fsS "$URL/api/v1/logs" | grep -q 'top-secret'
echo 'mock: ok'
