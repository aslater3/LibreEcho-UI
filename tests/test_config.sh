#!/bin/sh
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CFG=${LIBREECHO_TEST_CONFIG:-./build/test-suite-config.json}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
# This check runs both before setup and after setup has enabled AirPlay.
# Preserve existing integration bits while enabling Home Assistant (1) and
# Spotify (32) below; unrelated configuration writes must not reset them.
initial_integrations=$(curl -fsS "$URL/api/v1/config/export" | jq -er '.data.integrations')
expected_integrations=$((initial_integrations | 1 | 32))
curl -fsS -X PUT "$URL/api/v1/audio" -H "$CSRF" -H 'Content-Type: application/json' --data '{"volume":37}' >/dev/null
curl -fsS -X PUT "$URL/api/v1/led" -H "$CSRF" -H 'Content-Type: application/json' --data '{"r":12,"g":34,"b":56,"brightness":43,"visualizer_enabled":false}' >/dev/null
curl -fsS -X PUT "$URL/api/v1/network" -H "$CSRF" -H 'Content-Type: application/json' --data '{"hostname":"persistent-echo","ssh":true,"api_lan":true}' >/dev/null
curl -fsS -X PUT "$URL/api/v1/buttons" -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"short_press":"Play / pause","long_press":"Reboot device","tones":false,"action":"sound","action_sounds":"action-1,action-2","action_brightness":33,"mute_brightness":44}' \
    | jq -e '.ok and .data.tones == false and .data.action == "sound" and .data.action_sounds == "action-1,action-2" and .data.action_brightness == 33 and .data.mute_brightness == 44' >/dev/null
jq -e '.button_short == "Play / pause" and .button_long == "Reboot device" and .button_action_sounds == "action-1,action-2" and .button_action_brightness == 33 and .button_mute_brightness == 44' "$CFG" >/dev/null
curl -fsS -X PUT "$URL/api/v1/privacy" -H "$CSRF" -H 'Content-Type: application/json' --data '{"local_only":true,"diagnostic_telemetry":false,"crash_reports":false,"audio_retention":"none","log_retention_hours":168}' >/dev/null
curl -fsS -X PUT "$URL/api/v1/voice-pipeline" -H "$CSRF" -H 'Content-Type: application/json' --data '{"mode":"custom","stt_wyoming_uri":"tcp://198.51.100.10:10300","stt_model":"whisper-small","tts_wyoming_uri":"tcp://198.51.100.10:10200","tts_voice":"en_GB-alan-medium"}' >/dev/null
curl -fsS -X PUT "$URL/api/v1/integrations/home-assistant" -H "$CSRF" -H 'Content-Type: application/json' --data '{"enabled":true}' >/dev/null
curl -fsS "$URL/api/v1/voice-pipeline" |
    jq -e '.ok and .data.mode == "home-assistant"' >/dev/null
jq -e '.voice_pipeline_mode == "home-assistant" and .voice_pipeline_previous_mode == "custom"' "$CFG" >/dev/null
curl -fsS -X PUT "$URL/api/v1/integrations/home-assistant" -H "$CSRF" -H 'Content-Type: application/json' --data '{"enabled":false}' >/dev/null
curl -fsS "$URL/api/v1/voice-pipeline" |
    jq -e '.ok and .data.mode == "custom"' >/dev/null
jq -e '.voice_pipeline_mode == "custom"' "$CFG" >/dev/null
curl -fsS -X PUT "$URL/api/v1/integrations/home-assistant" -H "$CSRF" -H 'Content-Type: application/json' --data '{"enabled":true}' >/dev/null
curl -fsS -X PUT "$URL/api/v1/integrations/spotify" -H "$CSRF" -H 'Content-Type: application/json' --data '{"enabled":true}' >/dev/null
grep -q '"volume": 37' "$CFG"
jq -e --argjson integrations "$expected_integrations" '
  .hostname_persisted == true and .hostname == "persistent-echo" and
  .led_r == 12 and .led_g == 34 and .led_b == 56 and
  .led_brightness == 43 and .led_visualizer_enabled == false and
  .ssh == true and .api_lan == true and
  .button_short == "Play / pause" and .button_long == "Reboot device" and
  .button_tones == false and .button_action == "sound" and
  .button_action_brightness == 33 and .button_mute_brightness == 44 and
  .privacy_log_hours == 168 and .integrations == $integrations and
  .privacy_local_only == false and
  .voice_pipeline_mode == "home-assistant" and
  .stt_wyoming_uri == "tcp://198.51.100.10:10300" and
  .stt_wyoming_model == "whisper-small" and
  .tts_wyoming_uri == "tcp://198.51.100.10:10200" and
  .tts_wyoming_voice == "en_GB-alan-medium"
' "$CFG" >/dev/null
! grep -qi 'password' "$(dirname "$CFG")/test-suite-config.json"
curl -fsS -X PUT "$URL/api/v1/system/features" -H "$CSRF" \
    -H 'Content-Type: application/json' --data '{"acoustic_events":true}' >/dev/null
jq -e '.feature_acoustic_events == true' "$CFG" >/dev/null
exported=$(curl -fsS "$URL/api/v1/config/export" | jq -c '.data')
printf '%s' "$exported" | grep -q '"schema_version":1'
printf '%s' "$exported" | grep -q '"hostname_persisted":true'
printf '%s' "$exported" | jq -e \
    '.partial == false and .unsupported == [] and
     .led_visualizer_enabled == false and .button_tones == false and
     .feature_acoustic_events == true and
     .button_action == "sound" and .button_action_brightness == 33 and
     .button_action_sounds == "action-1,action-2" and
     .button_mute_brightness == 44' >/dev/null
! printf '%s' "$exported" | grep -Eqi 'password|auth_token|telemetry_value|logs'
! printf '%s' "$exported" | grep -q '"feature_usb_host"'
curl -fsS -X PUT "$URL/api/v1/system/features" -H "$CSRF" \
    -H 'Content-Type: application/json' --data '{"acoustic_events":false}' >/dev/null
duplicate_import="${exported%?},\"feature_acoustic_events\":\"enabled\"}"
code=$(curl -sS -o /tmp/le-duplicate-acoustic-import.out -w '%{http_code}' \
    -X POST "$URL/api/v1/config/import" -H "$CSRF" \
    -H 'Content-Type: application/json' --data "$duplicate_import")
[ "$code" = 400 ]
duplicate_import="${exported%?},\"feature_acoustic\u005fevents\":\"enabled\"}"
code=$(curl -sS -o /tmp/le-escaped-duplicate-acoustic-import.out -w '%{http_code}' \
    -X POST "$URL/api/v1/config/import" -H "$CSRF" \
    -H 'Content-Type: application/json' --data "$duplicate_import")
[ "$code" = 400 ]
curl -fsS "$URL/api/v1/system/features" |
    jq -e '.ok and .data.acoustic_events == false' >/dev/null
nested_export=$(printf '%s' "$exported" | jq -c 'del(.feature_acoustic_events) | .metadata={"feature_acoustic_events":true}')
curl -fsS -X POST "$URL/api/v1/config/import" -H "$CSRF" \
    -H 'Content-Type: application/json' --data "$nested_export" >/dev/null
curl -fsS "$URL/api/v1/system/features" |
    jq -e '.ok and .data.acoustic_events == false' >/dev/null
curl -fsS -X POST "$URL/api/v1/config/import" -H "$CSRF" \
    -H 'Content-Type: application/json' --data "$exported" >/dev/null
curl -fsS "$URL/api/v1/system/features" |
    jq -e '.ok and .data.acoustic_events == true and .data.acoustic_events_available == false' >/dev/null
curl -fsS -X PUT "$URL/api/v1/audio" -H "$CSRF" -H 'Content-Type: application/json' --data '{"volume":10}' >/dev/null
curl -fsS -X PUT "$URL/api/v1/buttons" -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"tones":true,"action":"disabled","action_sounds":"action-3","action_brightness":1,"mute_brightness":2}' >/dev/null
curl -fsS -X POST "$URL/api/v1/config/import" -H "$CSRF" -H 'Content-Type: application/json' --data "$exported" >/dev/null
curl -fsS "$URL/api/v1/audio" | grep -q '"volume":37'
curl -fsS "$URL/api/v1/buttons" | jq -e \
    '.data.tones == false and .data.action == "sound" and
     .data.action_sounds == "action-1,action-2" and
     .data.action_brightness == 33 and .data.mute_brightness == 44' >/dev/null
legacy_export=$(printf '%s' "$exported" | jq -c 'del(.led_visualizer_enabled)')
curl -fsS -X POST "$URL/api/v1/config/import" -H "$CSRF" \
    -H 'Content-Type: application/json' --data "$legacy_export" >/dev/null
curl -fsS "$URL/api/v1/led" | jq -e '.data.visualizer_enabled == true' >/dev/null
curl -fsS -X POST "$URL/api/v1/config/import" -H "$CSRF" \
    -H 'Content-Type: application/json' --data "$exported" >/dev/null
curl -fsS "$URL/api/v1/led" | jq -e '.data.visualizer_enabled == false' >/dev/null
code=$(curl -sS -o /tmp/le-bad-import.out -w '%{http_code}' -X POST "$URL/api/v1/config/import" -H "$CSRF" -H 'Content-Type: application/json' --data '{"schema_version":1,"volume":999}')
[ "$code" = 400 ]
for body in \
    '{"tones":"yes"}' \
    '{"action":"bogus"}' \
    '{"action_brightness":101}' \
    '{"mute_brightness":-1}' \
    '{"short_press":true}' \
    '{"long_press":123}' \
    '{"short_press":"12345678901234567890123456789012"}' \
    '{"long_press":"12345678901234567890123456789012"}'; do
    code=$(curl -sS -o /tmp/le-invalid-button-setting.out -w '%{http_code}' \
        -X PUT "$URL/api/v1/buttons" -H "$CSRF" -H 'Content-Type: application/json' --data "$body")
    [ "$code" = 400 ]
done
# #188: appearance is cosmetic and public, saved in web-config, and strict.
curl -fsS "$URL/api/v1/config" | jq -e '.data.ui_theme == "classic" and .data.color_mode == ""' >/dev/null
curl -fsS "$URL/api/v1/ui/preferences" | jq -e '.ok and .data.ui_theme == "classic" and .data.color_mode == ""' >/dev/null
curl -fsS -X PUT "$URL/api/v1/ui/preferences" -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"ui_theme":"setup"}' | jq -e '.data.ui_theme == "setup" and .data.color_mode == ""' >/dev/null
curl -fsS -X PUT "$URL/api/v1/ui/preferences" -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"color_mode":"light"}' | jq -e '.data.ui_theme == "setup" and .data.color_mode == "light"' >/dev/null
jq -e '.ui_theme == "setup" and .color_mode == "light"' "$CFG" >/dev/null
curl -fsS "$URL/api/v1/config" | jq -e '.data.ui_theme == "setup" and .data.color_mode == "light"' >/dev/null
for body in '{}' '{"ui_theme":"neon"}' '{"color_mode":"auto"}' '{"ui_theme":1}' \
            '{"ui_theme":"setup","extra":true}' '{"ui_theme":"setup","ui_theme":"classic"}' '[]'; do
    code=$(curl -sS -o /tmp/le-ui-prefs.out -w '%{http_code}' -X PUT "$URL/api/v1/ui/preferences" \
        -H "$CSRF" -H 'Content-Type: application/json' --data "$body")
    [ "$code" = 400 ]
done
code=$(curl -sS -o /tmp/le-ui-prefs-csrf.out -w '%{http_code}' -X PUT "$URL/api/v1/ui/preferences" \
    -H 'Content-Type: application/json' --data '{"ui_theme":"classic"}')
[ "$code" = 403 ]
curl -fsS "$URL/api/v1/ui/preferences" | jq -e '.data.ui_theme == "setup" and .data.color_mode == "light"' >/dev/null
# An empty color_mode means "not chosen on the device": GET reports it, so a PUT
# echoing GET (or clearing the choice) is accepted; other invalid values are not.
curl -fsS -X PUT "$URL/api/v1/ui/preferences" -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"color_mode":""}' | jq -e '.data.ui_theme == "setup" and .data.color_mode == ""' >/dev/null
curl -fsS "$URL/api/v1/ui/preferences" | jq -e '.data.ui_theme == "setup" and .data.color_mode == ""' >/dev/null
jq -e '.ui_theme == "setup" and .color_mode == ""' "$CFG" >/dev/null
prefs_get=$(curl -fsS "$URL/api/v1/ui/preferences" | jq -c '.data')
[ "$prefs_get" = '{"ui_theme":"setup","color_mode":""}' ]
curl -fsS -X PUT "$URL/api/v1/ui/preferences" -H "$CSRF" -H 'Content-Type: application/json' \
    --data "$prefs_get" | jq -e '.data.ui_theme == "setup" and .data.color_mode == ""' >/dev/null
curl -fsS -X PUT "$URL/api/v1/ui/preferences" -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"ui_theme":"classic","color_mode":""}' | jq -e '.data.ui_theme == "classic" and .data.color_mode == ""' >/dev/null
curl -fsS -X PUT "$URL/api/v1/ui/preferences" -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"ui_theme":"setup","color_mode":"light"}' | jq -e '.data.ui_theme == "setup" and .data.color_mode == "light"' >/dev/null
for body in '{"color_mode":"neon"}' '{"color_mode":"blue"}' '{"ui_theme":"classic","color_mode":"neon"}' \
            '{"ui_theme":"","color_mode":""}' '{"color_mode":" "}'; do
    code=$(curl -sS -o /tmp/le-ui-prefs.out -w '%{http_code}' -X PUT "$URL/api/v1/ui/preferences" \
        -H "$CSRF" -H 'Content-Type: application/json' --data "$body")
    [ "$code" = 400 ]
done
curl -fsS "$URL/api/v1/ui/preferences" | jq -e '.data.ui_theme == "setup" and .data.color_mode == "light"' >/dev/null
prefs_export=$(curl -fsS "$URL/api/v1/config/export" | jq -c '.data')
printf '%s' "$prefs_export" | jq -e '.ui_theme == "setup" and .color_mode == "light"' >/dev/null
curl -fsS -X POST "$URL/api/v1/config/import" -H "$CSRF" -H 'Content-Type: application/json' \
    --data "$(printf '%s' "$prefs_export" | jq -c '.ui_theme = "classic" | .color_mode = "dark"')" >/dev/null
curl -fsS "$URL/api/v1/ui/preferences" | jq -e '.data.ui_theme == "classic" and .data.color_mode == "dark"' >/dev/null
code=$(curl -sS -o /tmp/le-ui-prefs-import.out -w '%{http_code}' -X POST "$URL/api/v1/config/import" \
    -H "$CSRF" -H 'Content-Type: application/json' \
    --data "$(printf '%s' "$prefs_export" | jq -c '.ui_theme = "neon"')")
[ "$code" = 400 ]
curl -fsS "$URL/api/v1/ui/preferences" | jq -e '.data.ui_theme == "classic"' >/dev/null
# #64: the optional room is a device setting owned by the web daemon, so it
# persists, exports and restores without agentd, and bad input changes nothing.
curl -fsS "$URL/api/v1/device" | jq -e '.ok and .data.room == ""' >/dev/null
curl -fsS -X PUT "$URL/api/v1/device" -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"room":"Kitchen"}' | jq -e '.ok and .data.room == "Kitchen"' >/dev/null
jq -e '.room == "Kitchen"' "$CFG" >/dev/null
curl -fsS "$URL/api/v1/config/export" | jq -e '.data.room == "Kitchen"' >/dev/null
for body in '{"room":"0123456789012345678901234567890123456789012345678"}' \
            '{"room":"Hall\nNext"}' '{"room":"Say \"hi\""}' '{"room":7}' '{}'; do
    code=$(curl -sS -o /tmp/le-room-invalid.out -w '%{http_code}' -X PUT "$URL/api/v1/device" \
        -H "$CSRF" -H 'Content-Type: application/json' --data "$body")
    [ "$code" = 400 ]
done
code=$(curl -sS -o /tmp/le-room-csrf.out -w '%{http_code}' -X PUT "$URL/api/v1/device" \
    -H 'Content-Type: application/json' --data '{"room":"Hall"}')
[ "$code" = 403 ]
curl -fsS "$URL/api/v1/device" | jq -e '.data.room == "Kitchen"' >/dev/null
# Exactly 48 bytes is accepted; multi-byte UTF-8 counts in bytes.
curl -fsS -X PUT "$URL/api/v1/device" -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"room":"012345678901234567890123456789012345678901234567"}' >/dev/null
curl -fsS -X PUT "$URL/api/v1/device" -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"room":"Kinderzimmer – oben"}' | jq -e '.data.room == "Kinderzimmer – oben"' >/dev/null
room_export=$(curl -fsS "$URL/api/v1/config/export" | jq -c '.data | .room = "Hall"')
curl -fsS -X POST "$URL/api/v1/config/import" -H "$CSRF" \
    -H 'Content-Type: application/json' --data "$room_export" >/dev/null
curl -fsS "$URL/api/v1/device" | jq -e '.data.room == "Hall"' >/dev/null
bad_room_export=$(printf '%s' "$room_export" | jq -c '.room = "Hall\nNext"')
code=$(curl -sS -o /tmp/le-room-import.out -w '%{http_code}' -X POST "$URL/api/v1/config/import" \
    -H "$CSRF" -H 'Content-Type: application/json' --data "$bad_room_export")
[ "$code" = 400 ]
curl -fsS "$URL/api/v1/device" | jq -e '.data.room == "Hall"' >/dev/null
curl -fsS -X PUT "$URL/api/v1/device" -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"room":""}' | jq -e '.data.room == ""' >/dev/null
jq -e '.room == ""' "$CFG" >/dev/null
mode=$(stat -c '%a' "$CFG" 2>/dev/null || stat -f '%Lp' "$CFG")
[ "$mode" = 600 ]
echo 'config: ok'
