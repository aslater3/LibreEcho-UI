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
sleep 3
curl -fsS "$URL/api/v1/network" | grep -q '"ssid":"LibreNet-IoT"'
! curl -fsS "$URL/api/v1/logs" | grep -q 'top-secret'
# Wake-word capture health is derived from successive samples of the 64-bit
# frame counter, not from VAD: a healthy quiet-room mock reports vad_active
# false with capture_active true, and the counter (already beyond INT_MAX)
# must advance exactly, without wrapping.
frames_before=$(curl -fsS "$URL/api/v1/wake-word" | jq -r '.data.processed_frames')
sleep 1.2
wake=$(curl -fsS "$URL/api/v1/wake-word")
printf '%s' "$wake" | jq -e \
    '.data.model_loaded == true and .data.vad_active == false and
     .data.capture_active == true and .data.processed_frames > 2147483647' >/dev/null
frames_after=$(printf '%s' "$wake" | jq -r '.data.processed_frames')
[ "$frames_after" -gt "$frames_before" ]
# A frozen counter with the model still loaded is the post-playback stall:
# successive samples must stop calling capture active.
ctl stall-wake-capture
curl -fsS "$URL/api/v1/wake-word" >/dev/null
sleep 1.2
curl -fsS "$URL/api/v1/wake-word" | jq -e \
    '.data.model_loaded == true and .data.capture_active == false' >/dev/null
ctl resume-wake-capture
echo 'wake capture health: ok'
echo 'mock: ok'
