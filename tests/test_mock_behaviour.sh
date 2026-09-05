#!/bin/sh
set -eu
stage=initialization
trap 'rc=$?; if [ "$rc" -ne 0 ]; then printf "mock behaviour failed: %s\n" "$stage" >&2; fi' EXIT
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
ctl(){ curl -fsS -X POST "$URL/api/v1/dev/mock" -H "$CSRF" -H 'Content-Type: application/json' --data "{\"action\":\"$1\",\"value\":\"${2:-}\"}" >/dev/null; }
stage=temperature
ctl set-temperature 72
curl -fsS "$URL/api/v1/status" | grep -Eq '"temperature_c":(69|70|71|72|73|74|75)'
stage=scan-fault
ctl fail-next wifi-scan
code=$(curl -sS -o /tmp/le-fault.out -w '%{http_code}' "$URL/api/v1/network/wifi/scan")
[ "$code" = 501 ]
stage=wake-trigger
ctl trigger wake-word
curl -fsS "$URL/api/v1/wake-word" | grep -q '"detected_count":1'
curl -fsS -X POST "$URL/api/v1/network/wifi/connect" -H "$CSRF" -H 'Content-Type: application/json' --data '{"ssid":"LibreNet-IoT","password":"top-secret","security":"wpa2"}' >/dev/null
stage=wifi-completion
i=0
while ! curl -fsS "$URL/api/v1/network" | jq -e '.data.state == "connected" and .data.ssid == "LibreNet-IoT"' >/dev/null; do
    i=$((i + 1))
    if [ "$i" -ge 50 ]; then
        curl -fsS "$URL/api/v1/network" | jq '.data | {state, connectivity}' >&2
        exit 1
    fi
    sleep 0.1
done
stage=log-redaction
! curl -fsS "$URL/api/v1/logs" | grep -q 'top-secret'
echo 'mock: ok'
