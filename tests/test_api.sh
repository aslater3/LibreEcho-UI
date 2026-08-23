#!/bin/sh
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CFG=${LIBREECHO_TEST_CONFIG:-./build/test-suite-config.json}
OS_VERSION=$(tr -d '\r\n' < VERSION)
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
expect(){ printf '%s' "$1" | grep -q "$2" || { echo "expected $2 in $1" >&2; exit 1; }; }
expect "$(curl -fsS "$URL/api/v1/status")" '"backend":"mock"'
curl -fsS "$URL/api/v1/network" | jq -e '.ok and .data.connectivity == "healthy" and .data.recovery_stage == "none" and .data.gateway_reachable == true and .data.liveness_failures == 0' >/dev/null
expect "$(curl -fsS "$URL/api/v1/device")" "\"os_version\":\"LibreEcho OS $OS_VERSION\""
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
code=$(curl -sS -o /tmp/le-invalid-wifi-security.out -w '%{http_code}' \
    -X POST "$URL/api/v1/network/wifi/connect" -H "$CSRF" \
    -H 'Content-Type: application/json' \
    --data '{"ssid":"LibreNet-IoT","password":"top-secret","security":"none"}')
[ "$code" = 400 ]
code=$(curl -sS -o /tmp/le-malformed-wifi-security.out -w '%{http_code}' \
    -X POST "$URL/api/v1/network/wifi/connect" -H "$CSRF" \
    -H 'Content-Type: application/json' \
    --data '{"ssid":"LibreNet-IoT","password":"top-secret","security":"open\\nrest"}')
[ "$code" = 400 ]
! grep -q 'top-secret' "$CFG"
curl -fsS "$URL/openapi.json" | grep -Eq '"openapi"[[:space:]]*:[[:space:]]*"3.0.3"'
expect "$(curl -fsS "$URL/swagger.html")" 'API reference · LibreEcho'
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
curl -fsS -X PUT "$URL/api/v1/led/profile" -H "$CSRF" \
    -H 'Content-Type: application/json' \
    --data '{"name":"listening","r":72,"g":216,"b":118,"brightness":80}' | \
    jq -e '.ok and .data.profiles.listening != null' >/dev/null
code=$(curl -sS -o /tmp/le-invalid-led-profile.out -w '%{http_code}' \
    -X PUT "$URL/api/v1/led/profile" -H "$CSRF" \
    -H 'Content-Type: application/json' \
    --data '{"name":"unknown","r":72,"g":216,"b":118}')
[ "$code" = 400 ]
curl -fsS "$URL/api/v1/baby-monitor" | jq -e \
    '.ok and .data.sources[0].channels == 1 and
     .data.sources[0].valid_bits == 16 and
     .data.active_microphone_channels == 7 and
     .data.inactive_transport_channels == [7,8] and
     .data.calibration.fallback == 16384 and
     .data.calibration.selected_logical_mics == [0,3] and
     .data.calibration.applied_to_raw_stream == false' >/dev/null
code=$(curl -sS -o /tmp/le-baby-stream.out -w '%{http_code}' "$URL/api/v1/baby-monitor/stream?source=0:0")
[ "$code" = 501 ]
code=$(curl -sS -o /tmp/le-baby-stream-encoded.out -w '%{http_code}' "$URL/api/v1/baby-monitor/stream?source=0%3A24&channel=0")
[ "$code" = 501 ]
grep -q 'not_supported' /tmp/le-baby-stream-encoded.out
code=$(curl -sS -o /tmp/le-baby-stream-invalid-encoding.out -w '%{http_code}' "$URL/api/v1/baby-monitor/stream?source=0%ZZ24&channel=0")
[ "$code" = 400 ]
grep -q 'invalid_request' /tmp/le-baby-stream-invalid-encoding.out
curl -fsS "$URL/api/v1/bluetooth" | jq -e \
    '.data.profile_state == "pairing-only" and
     (.data.profile_error | length) > 0 and
     (.data.profile_services.sdp == false) and
     (.data.profile_services.a2dp_sink == false) and
     (.data.profile_services.avrcp == false) and
     (.data.profile_services.rfcomm == false) and
     (.data.profile_services.bnep == false) and
     (.data.profile_services.hidp == false)' >/dev/null
curl -fsS -X PUT "$URL/api/v1/bluetooth" -H "$CSRF" -H 'Content-Type: application/json' --data '{"enabled":true}' | jq -e '.ok and .data.enabled == true' >/dev/null
curl -fsS -X PUT "$URL/api/v1/bluetooth" -H "$CSRF" -H 'Content-Type: application/json' --data '{"connectable":false}' | jq -e '.ok and .data.capabilities.connectable == false' >/dev/null
curl -fsS -X PUT "$URL/api/v1/bluetooth" -H "$CSRF" -H 'Content-Type: application/json' --data '{"discoverable":true}' | jq -e '.ok and .data.capabilities.discoverable == true' >/dev/null
curl -fsS -X POST "$URL/api/v1/bluetooth/scan" -H "$CSRF" -H 'Content-Type: application/json' --data '{}' | jq -e '.ok and .data.scanning == true' >/dev/null
curl -fsS -X POST "$URL/api/v1/bluetooth/pair" -H "$CSRF" -H 'Content-Type: application/json' --data '{"address":"10:20:30:40:50:60","type":0,"io_capability":3}' | jq -e '.ok and (.data.known_devices | length) == 1' >/dev/null
curl -fsS -X POST "$URL/api/v1/bluetooth/unpair" -H "$CSRF" -H 'Content-Type: application/json' --data '{"address":"10:20:30:40:50:60","type":0}' | jq -e '.ok and (.data.known_devices | length) == 0' >/dev/null
curl -fsS "$URL/api/v1/network" | jq -e '.ok and (.data.connectivity == "unknown" or .data.connectivity == "healthy") and .data.recovery_stage == "none" and ((.data.gateway_reachable | type) == "boolean" or .data.gateway_reachable == null) and .data.liveness_failures == 0' >/dev/null
expect "$(curl -fsS "$URL/api/v1/network/wifi/scan")" 'LibreNet-5G'
curl -fsS "$URL/api/v1/voice-pipeline" | jq -e \
    '.ok and .data.mode == "local" and
     .data.stt.engine == "sherpa" and
     .data.tts.engine == "sherpa" and
     .data.wake_word.processing == "on-device"' >/dev/null
curl -fsS -X PUT "$URL/api/v1/voice-pipeline" -H "$CSRF" \
    -H 'Content-Type: application/json' \
    --data '{"mode":"local","max_utterance_ms":6000,"end_silence_ms":1500,"vad_floor_rms":45}' |
    jq -e '.ok and .data.listening.max_utterance_ms == 6000 and
           .data.listening.end_silence_ms == 1500 and
           .data.listening.vad_floor_rms == 45' >/dev/null
code=$(curl -sS -o /tmp/le-invalid-listening-type.out -w '%{http_code}' \
    -X PUT "$URL/api/v1/voice-pipeline" -H "$CSRF" \
    -H 'Content-Type: application/json' \
    --data '{"mode":"local","max_utterance_ms":"7000"}')
[ "$code" = 400 ]
code=$(curl -sS -o /tmp/le-invalid-listening-atomic.out -w '%{http_code}' \
    -X PUT "$URL/api/v1/voice-pipeline" -H "$CSRF" \
    -H 'Content-Type: application/json' \
    --data '{"mode":"invalid","max_utterance_ms":7000}')
[ "$code" = 400 ]
curl -fsS "$URL/api/v1/voice-pipeline" |
    jq -e '.data.listening.max_utterance_ms == 6000' >/dev/null
curl -fsS -X PUT "$URL/api/v1/voice-pipeline" -H "$CSRF" \
    -H 'Content-Type: application/json' \
    --data '{"mode":"custom","stt_wyoming_uri":"tcp://127.0.0.1:10300","stt_model":"whisper-small","tts_wyoming_uri":"tcp://127.0.0.1:10200","tts_voice":"en_GB-alan-medium"}' |
    jq -e '.ok and .data.mode == "custom" and
           .data.stt.engine == "wyoming" and
           .data.tts.engine == "wyoming"' >/dev/null
curl -fsS "$URL/api/v1/privacy" |
    jq -e '.ok and .data.local_only == false' >/dev/null
code=$(curl -sS -o /tmp/le-local-only-conflict.out -w '%{http_code}' \
    -X PUT "$URL/api/v1/privacy" -H "$CSRF" \
    -H 'Content-Type: application/json' --data '{"local_only":true}')
[ "$code" = 409 ]
curl -fsS -X PUT "$URL/api/v1/voice-pipeline" -H "$CSRF" \
    -H 'Content-Type: application/json' --data '{"mode":"local"}' >/dev/null
curl -fsS -X PUT "$URL/api/v1/privacy" -H "$CSRF" \
    -H 'Content-Type: application/json' --data '{"local_only":true}' |
    jq -e '.ok and .data.local_only == true' >/dev/null
code=$(curl -sS -o /tmp/le-invalid-pipeline.out -w '%{http_code}' \
    -X PUT "$URL/api/v1/voice-pipeline" -H "$CSRF" \
    -H 'Content-Type: application/json' \
    --data '{"mode":"custom","stt_wyoming_uri":"http://invalid","tts_wyoming_uri":"tcp://127.0.0.1:10200","stt_model":"whisper-small","tts_voice":"en_GB-alan-medium"}')
[ "$code" = 400 ]
expect "$(curl -fsS "$URL/api/v1/system")" '"ntp":false'
expect "$(curl -fsS "$URL/api/v1/system")" '"clock_valid":true'
expect "$(curl -fsS "$URL/api/v1/system")" '"ntp_state":"unavailable"'
expect "$(curl -fsS "$URL/api/v1/system")" '"rtc_available":false'
curl -fsS "$URL/api/v1/system/update" | jq -e \
    '.ok and .data.supported == false and
     .data.current_slot == "-" and .data.inactive_slot == "-" and
     .data.pending_reboot == false and .data.max_upload_bytes == 33554432 and
     .data.installed_version == "" and .data.latest_version == "" and
     .data.channel == "stable" and .data.source == "github-releases" and
     .data.source_reachable == "unknown" and
     .data.check_status == "not-checked" and .data.check_error == "" and
     .data.last_check_epoch == 0 and .data.last_success_epoch == 0 and
     .data.automatic_updates == false and
     .data.rollback_version == ""' \
    >/dev/null
for endpoint in check apply; do
    code=$(curl -sS -o "/tmp/le-update-$endpoint.out" -w '%{http_code}' \
        -X POST "$URL/api/v1/system/update/$endpoint" -H "$CSRF" \
        -H 'Content-Type: application/json' --data '{}')
    [ "$code" = 501 ]
done
code=$(curl -sS -o /tmp/le-update-channel.out -w '%{http_code}' \
    -X PUT "$URL/api/v1/system/update/channel" -H "$CSRF" \
    -H 'Content-Type: application/json' --data '{"channel":"stable"}')
[ "$code" = 501 ]
code=$(curl -sS -o /tmp/le-update-channel-invalid.out -w '%{http_code}' \
    -X PUT "$URL/api/v1/system/update/channel" -H "$CSRF" \
    -H 'Content-Type: application/json' --data '{"channel":"bogus"}')
[ "$code" = 501 ]
code=$(curl -sS -o /tmp/le-update-upload.out -w '%{http_code}' \
    -X POST "$URL/api/v1/system/update/upload" -H "$CSRF" \
    -H 'Content-Type: application/x-tar' --data-binary 'not-an-update')
[ "$code" = 501 ]
code=$(curl -sS -o /tmp/le-update-automatic.out -w '%{http_code}' \
    -X PUT "$URL/api/v1/system/update/automatic" -H "$CSRF" \
    -H 'Content-Type: application/json' --data '{"enabled":true}')
[ "$code" = 501 ]
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
# Announce API: mock backend returns 501 (not supported), but the endpoint
# must exist and reject empty text with 400.
code=$(curl -sS -o /tmp/le-announce-empty.out -w '%{http_code}' -X POST "$URL/api/v1/audio/announce" -H "$CSRF" -H 'Content-Type: application/json' --data '{}')
[ "$code" = 400 ]
code=$(curl -sS -o /tmp/le-announce-mock.out -w '%{http_code}' -X POST "$URL/api/v1/audio/announce" -H "$CSRF" -H 'Content-Type: application/json' --data '{"text":"Now playing \"Don’t Look Back in Anger\" by Oasis"}')
[ "$code" = 501 ]
code=$(curl -sS -o /tmp/le-announce-stop.out -w '%{http_code}' -X POST "$URL/api/v1/audio/announce/stop" -H "$CSRF" -H 'Content-Type: application/json' --data '{}')
[ "$code" = 501 ]
curl -fsS -X PUT "$URL/api/v1/audio" -H "$CSRF" -H 'Content-Type: application/json' --data '{"tts_voice":"northern-male"}' | grep -q '"tts_voice":"northern-male"'
code=$(curl -sS -o /tmp/le-voice-invalid.out -w '%{http_code}' -X PUT "$URL/api/v1/audio" -H "$CSRF" -H 'Content-Type: application/json' --data '{"tts_voice":"not-a-voice"}')
[ "$code" = 400 ]
echo 'api: ok'
