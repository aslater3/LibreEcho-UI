#!/bin/sh
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CFG=${LIBREECHO_TEST_CONFIG:-./build/test-suite-config.json}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
curl -fsS -X PUT "$URL/api/v1/audio" -H "$CSRF" -H 'Content-Type: application/json' --data '{"volume":37}' >/dev/null
curl -fsS -X PUT "$URL/api/v1/led" -H "$CSRF" -H 'Content-Type: application/json' --data '{"r":12,"g":34,"b":56,"brightness":43,"visualizer_enabled":false}' >/dev/null
curl -fsS -X PUT "$URL/api/v1/network" -H "$CSRF" -H 'Content-Type: application/json' --data '{"hostname":"persistent-echo","ssh":true,"api_lan":true}' >/dev/null
curl -fsS -X PUT "$URL/api/v1/buttons" -H "$CSRF" -H 'Content-Type: application/json' --data '{"short_press":"Play / pause","long_press":"Reboot device"}' >/dev/null
curl -fsS -X PUT "$URL/api/v1/privacy" -H "$CSRF" -H 'Content-Type: application/json' --data '{"local_only":true,"diagnostic_telemetry":false,"crash_reports":false,"audio_retention":"none","log_retention_hours":168}' >/dev/null
curl -fsS -X PUT "$URL/api/v1/integrations/home-assistant" -H "$CSRF" -H 'Content-Type: application/json' --data '{"enabled":true}' >/dev/null
grep -q '"volume": 37' "$CFG"
jq -e '
  .hostname_persisted == true and .hostname == "persistent-echo" and
  .led_r == 12 and .led_g == 34 and .led_b == 56 and
  .led_brightness == 43 and .led_visualizer_enabled == false and
  .ssh == true and .api_lan == true and
  .button_short == "Play / pause" and .button_long == "Reboot device" and
  .privacy_log_hours == 168 and .integrations == 5
' "$CFG" >/dev/null
! grep -qi 'password' "$(dirname "$CFG")/test-suite-config.json"
exported=$(curl -fsS "$URL/api/v1/config/export" | jq -c '.data')
printf '%s' "$exported" | grep -q '"schema_version":1'
printf '%s' "$exported" | grep -q '"hostname_persisted":true'
printf '%s' "$exported" | jq -e \
    '.partial == false and .unsupported == [] and
     .led_visualizer_enabled == false' >/dev/null
! printf '%s' "$exported" | grep -Eqi 'password|auth_token|telemetry_value|logs'
curl -fsS -X PUT "$URL/api/v1/audio" -H "$CSRF" -H 'Content-Type: application/json' --data '{"volume":10}' >/dev/null
curl -fsS -X POST "$URL/api/v1/config/import" -H "$CSRF" -H 'Content-Type: application/json' --data "$exported" >/dev/null
curl -fsS "$URL/api/v1/audio" | grep -q '"volume":37'
legacy_export=$(printf '%s' "$exported" | jq -c 'del(.led_visualizer_enabled)')
curl -fsS -X POST "$URL/api/v1/config/import" -H "$CSRF" \
    -H 'Content-Type: application/json' --data "$legacy_export" >/dev/null
curl -fsS "$URL/api/v1/led" | jq -e '.data.visualizer_enabled == true' >/dev/null
curl -fsS -X POST "$URL/api/v1/config/import" -H "$CSRF" \
    -H 'Content-Type: application/json' --data "$exported" >/dev/null
curl -fsS "$URL/api/v1/led" | jq -e '.data.visualizer_enabled == false' >/dev/null
code=$(curl -sS -o /tmp/le-bad-import.out -w '%{http_code}' -X POST "$URL/api/v1/config/import" -H "$CSRF" -H 'Content-Type: application/json' --data '{"schema_version":1,"volume":999}')
[ "$code" = 400 ]
mode=$(stat -c '%a' "$CFG" 2>/dev/null || stat -f '%Lp' "$CFG")
[ "$mode" = 600 ]
echo 'config: ok'
