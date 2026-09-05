#!/bin/sh
set -eu
PORT=${LIBREECHO_TEST_PORT:-18082}
URL="http://127.0.0.1:$PORT"
CFG=./build/test-suite-config.json
rm -f "$CFG" "$CFG.bak" "$CFG.tmp" "$CFG.setup-complete"
cc -D_POSIX_C_SOURCE=200809L -std=c99 -Isrc tests/test_unit.c src/json.c src/config_store.c -o build/test-unit
./build/test-unit
python3 tests/test_github_link_contract.py
python3 tests/test_setup_wifi_ui_contract.py
make build/test-network-health build/test-adapter-client-events build/test-gateway-probe build/test-networkd-health build/test-networkd-scan-security build/test-bt-mgmt-events build/test-bt-pairing-events build/test-factory-reset
./build/test-networkd-scan-security
./build/test-network-health
./build/test-adapter-client-events
./build/test-gateway-probe
./build/test-factory-reset
sh tests/test_factory_reset_bluetooth_contract.sh
sh tests/test_factory_reset_quiesce_contract.sh
make build/test-backend-linux-wifi-emission
./build/test-backend-linux-wifi-emission
./build/test-bt-mgmt-events
./build/test-bt-pairing-events
python3 tests/test_networkd_health_integration.py
python3 tests/test_backend_linux_wifi_contract.py
sh tests/test_network_liveness_contract.sh
sh tests/test_init_service_control.sh
sh tests/test_bluetooth_pairing_contract.sh
sh tests/test_bluetooth_pairing_code_ui.sh
sh tests/test_bluetooth_io_capability_contract.sh
sh tests/test_bluetooth_profile_contract.sh
sh tests/test_bluetooth_profile_service_contract.sh
sh tests/test_bluetooth_device_metadata_contract.sh
sh tests/test_bluetooth_cache_bust_contract.sh
sh tests/test_bluetooth_mgmt_observability_contract.sh
make build/test-sdp-wire-format
./build/test-sdp-wire-format
make build/test-avdtp-wire-format
./build/test-avdtp-wire-format
sh tests/test_network_scan_contract.sh
sh tests/test_setup_account_first.sh
sh tests/test_setup_optional_adapters.sh
sh tests/test_login_brand_contract.sh
grep -q '"SAVE_CONFIG\\n"' src/adapter/networkd.c
sh tests/test_led_pattern_ownership.sh
python3 tests/test_buttond_led_restart.py
make build/test-action-sample build/test-audiod-review build/test-led-night-review
./build/test-action-sample
./build/test-audiod-review
./build/test-led-night-review
sh tests/test_startup_animation.sh
make build/test-button-settings build/test-buttond-privacy build/test-buttond-events build/test-buttond-timing
./build/test-button-settings
./build/test-buttond-privacy
./build/test-buttond-events
./build/test-buttond-timing
sh tests/test_buttond_contract.sh
sh tests/test_input_capability_state_contract.sh
sh tests/test_bluetooth_startup_readiness_contract.sh
sh tests/test_bluetooth_startup_optionality_contract.sh
sh tests/test_bluetooth_decoder_state_contract.sh
make build/test-wake-led
sh tests/test_microphone_fanout_contract.sh
sh tests/test_audio_retention_contract.sh
python3 tests/test_baby_monitor_stream_contract.py
python3 tests/test_startup_state_contract.py
python3 tests/test_wake_word_ui_contract.py
python3 tools/test_virtual_echo.py
node tests/test_wifi_security_interaction.js
node tests/test_timers_ui.js
python3 tests/test_issue_34.py
python3 tests/test_issue_94.py
python3 tests/voice-e2e/test_audio_quality.py
sh tests/test_device_identity.sh
# The CPU online-mask fixture requires a Linux sysfs-shaped runtime and is run
# separately; keep the aggregate suite deterministic across CI runners.
# sh tests/test_cpu_online_mask.sh
python3 tests/test_public_source_safety.py
python3 tests/test_diagnostic_export_contract.py
sh tests/test_source_provenance.sh
sh tests/test_ota_channel_contract.sh
sh tests/test_update_failure_contract.sh
sh tests/test_stt_listening_config_contract.sh
sh tests/test_pr95_followups_contract.sh
sh tests/test_setup_connectivity_contract.sh
sh tests/test_wake_led.sh
sh tests/test_led_visualizer.sh
sh tests/test_airplay_led_bridge.sh
sh tests/test_airplay_setup_persistence.sh
sh tests/test_airplay_mount_failure.sh
cc -D_POSIX_C_SOURCE=200809L -std=c99 -Wall -Wextra -Wpedantic -Werror \
    -Isrc -Isrc/adapter tests/test_airplay_metadata.c \
    src/adapter/adapter_client.c src/adapter/adapter_server.c src/log.c \
    -o build/test-airplay-metadata
./build/test-airplay-metadata
cc -D_POSIX_C_SOURCE=200809L -std=c99 -Wall -Wextra -Wpedantic -Werror \
    -Isrc -Isrc/adapter tests/test_micd.c \
    src/adapter/adapter_client.c src/adapter/adapter_server.c src/log.c \
    -o build/test-micd
./build/test-micd
cc -D_POSIX_C_SOURCE=200809L -std=c99 -Wall -Wextra -Wpedantic -Werror \
    -Isrc -Isrc/adapter tests/test_voice_dsp.c src/adapter/voice_dsp.c \
    -o build/test-voice-dsp
./build/test-voice-dsp
make build/test-voice-aec build/test-voice-reference
./build/test-voice-aec
./build/test-voice-reference
make build/test-voice-stream build/test-sttd build/test-llm-provider \
    build/test-llm-http build/test-llm-store build/test-agentd
make build/test-voice-reply build/test-voice-playback
make build/test-voice-pipeline
./build/test-voice-stream
./build/test-sttd
./build/test-llm-provider
./build/test-llm-http
./build/test-llm-store
./build/test-agentd
./build/test-voice-reply
./build/test-voice-playback
./build/test-voice-pipeline
make build/test-wyomingd
./build/test-wyomingd
python3 tests/test_wyoming_engines.py
cc -D_POSIX_C_SOURCE=200809L -std=c99 -Wall -Wextra -Wpedantic -Werror \
    -Isrc -Isrc/adapter tests/test_ttsd.c \
    src/adapter/adapter_client.c src/adapter/adapter_server.c src/log.c \
    -o build/test-ttsd
./build/test-ttsd
sh tests/test_timed.sh
sh tests/test_timed_timeout.sh
make build/test-timer-intent
./build/test-timer-intent
make build/test-timer-schedule
./build/test-timer-schedule
make build/test-timer-json
./build/test-timer-json
make build/test-backend-linux-timers
./build/test-backend-linux-timers
make build/test-backend-mock-timers
./build/test-backend-mock-timers
make build/test-timer-persistence build/libreecho-audiod build/libreecho-timerd
./build/test-timer-persistence
sh tests/test_timerd.sh
make build/libreecho-agentd
sh tests/test_agentd_startup_readiness_contract.sh
sh tests/test_agentd_timers.sh
./build/libreecho-web --backend mock --config "$CFG" --mock-config ./config/mock-state.json --web-root ./web --listen "127.0.0.1:$PORT" --seed 42 --dev-controls >./build/test-server.log 2>&1 &
pid=$!
cleanup(){ if [ "${pid:-0}" -gt 1 ]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi; }
trap cleanup EXIT INT TERM
i=0
while ! curl -fsS "$URL/api/v1/status" >/dev/null 2>&1; do i=$((i+1)); [ "$i" -lt 30 ] || { cat ./build/test-server.log; exit 1; }; sleep 0.1; done
LIBREECHO_TEST_URL="$URL" sh tests/test_api.sh
LIBREECHO_TEST_URL="$URL" sh tests/test_timers_api.sh
LIBREECHO_TEST_URL="$URL" sh tests/test_diagnostics_export.sh
LIBREECHO_TEST_URL="$URL" LIBREECHO_TEST_CONFIG="$CFG" sh tests/test_config.sh
LIBREECHO_TEST_URL="$URL" sh tests/test_mock_behaviour.sh
LIBREECHO_TEST_URL="$URL" sh tests/test_limits.sh
sh tests/test_memory.sh "$pid"
kill "$pid"
wait "$pid" 2>/dev/null || true
pid=0
sh tests/test_timers_linux_validation.sh
./tools/create-user.sh test-user test-password-123 >./build/test-users
chmod 600 ./build/test-users
./build/libreecho-web --backend mock --config "$CFG" --web-root ./web --listen "127.0.0.1:$PORT" --seed 42 --users-file ./build/test-users >./build/test-users.log 2>&1 &
pid=$!
sleep 1
LIBREECHO_TEST_URL="$URL" LIBREECHO_TEST_USERS=./build/test-users sh tests/test_auth.sh
kill "$pid"
wait "$pid" 2>/dev/null || true
pid=0
printf '{}\n' >./build/bootstrap-config.json
rm -f ./build/bootstrap-users ./build/test-bootstrap.log ./build/bootstrap-config.json.setup-complete
./build/libreecho-web --backend mock --config ./build/bootstrap-config.json --web-root ./web --listen "127.0.0.1:$PORT" --seed 42 --users-file ./build/bootstrap-users >./build/test-bootstrap.log 2>&1 &
pid=$!
sleep 1
LIBREECHO_TEST_URL="$URL" sh tests/test_auth_bootstrap.sh
kill "$pid"
wait "$pid" 2>/dev/null || true
pid=0
pid=0
./build/libreecho-web --backend mock --config "$CFG" --web-root ./web --listen "127.0.0.1:$PORT" --seed 42 >./build/test-restart.log 2>&1 &
pid=$!
sleep 1
curl -fsS "$URL/api/v1/audio" | grep -q '"volume":37'
curl -fsS "$URL/api/v1/device" | grep -q '"hostname":"persistent-echo"'
curl -fsS "$URL/api/v1/led" | jq -e \
    '.data.colour == {"r":12,"g":34,"b":56} and
     .data.brightness == 43 and .data.visualizer_enabled == false' \
    >/dev/null
curl -fsS "$URL/api/v1/buttons" | jq -e \
    '.data.short_press == "Play / pause" and
     .data.long_press == "Reboot device"' >/dev/null
curl -fsS "$URL/api/v1/privacy" | jq -e \
    '.data.local_only == false and .data.log_retention_hours == 168' >/dev/null
curl -fsS "$URL/api/v1/integrations" | jq -e \
    '.data.items[] | select(.id == "home-assistant") | .enabled == true' \
    >/dev/null
curl -fsS "$URL/api/v1/config" | grep -q '"setup_completed":true'
curl -fsS "$URL/" | grep -q 'LibreEcho Control Centre'
echo 'persistence: ok'
kill "$pid"
wait "$pid" 2>/dev/null || true
pid=0
cat >./build/test-time.status <<'EOF'
state=synchronized
source=ntp
synchronized=1
clock_valid=1
rtc_available=1
rtc_persisted=1
last_sync_epoch=1700000000
config_source=image
servers=time.cloudflare.com,time.nist.gov
EOF
cat >./build/test-vendor-import.status <<'EOF'
state=ready
verification=hash-pinned
source_partition=system_a
source_layout=etc/firmware
force_requested=0
error=none
EOF
mkdir -p ./build/test-vendor-config
rm -f ./build/test-vendor-config/vendor-import-force-next-boot
: >./build/test-wlan0
LIBREECHO_TIME_STATUS=./build/test-time.status \
LIBREECHO_VENDOR_STATUS_PATH=./build/test-vendor-import.status \
LIBREECHO_VENDOR_FORCE_MARKER=./build/test-vendor-config/vendor-import-force-next-boot \
LIBREECHO_WLAN0_PATH=./build/test-wlan0 \
./build/libreecho-web --backend linux --config "$CFG" --web-root ./web --listen "127.0.0.1:$PORT" >./build/test-linux.log 2>&1 &
pid=$!
i=0
while ! curl -sS "$URL/api/v1/config" >/dev/null 2>&1; do
    i=$((i + 1))
    [ "$i" -lt 200 ] || { printf '%s\n' 'Linux API validation server did not start' >&2; exit 1; }
    sleep 0.1
done
code=$(curl -sS -o /tmp/le-linux-audio.out -w '%{http_code}' "$URL/api/v1/audio")
[ "$code" = 200 ]
jq -e '.ok == true and .data.available == false and .data.unavailable == true' /tmp/le-linux-audio.out >/dev/null
code=$(curl -sS -o /tmp/le-linux-config.out -w '%{http_code}' "$URL/api/v1/config/export")
[ "$code" = 200 ]
jq -e '.ok == true and .data.partial == true and (.data.unsupported | index("wake_word")) != null' /tmp/le-linux-config.out >/dev/null
LIBREECHO_TEST_URL="$URL" sh tests/test_diagnostics_export_linux.sh
curl -fsS "$URL/api/v1/setup" | jq -e \
    '.data.vendor_firmware.state == "ready" and
     .data.vendor_firmware.verification == "hash-pinned" and
     .data.vendor_firmware.source_layout == "etc/firmware" and
     .data.vendor_firmware.force_next_boot == false and
     .data.wake_word == "LibreEcho" and
     .data.wlan0_registered == true' >/dev/null
mv ./build/test-vendor-import.status ./build/test-vendor-import.status.saved
curl -fsS "$URL/api/v1/setup" | jq -e \
    '.data.vendor_firmware.state == "unavailable" and
     .data.wlan0_registered == true and
     .data.wake_word == "LibreEcho"' >/dev/null
mv ./build/test-vendor-import.status.saved ./build/test-vendor-import.status
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
curl -fsS -X POST "$URL/api/v1/setup/vendor-import-force-next-boot" \
    -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"confirm":"force-unverified-owner-local-import"}' | jq -e \
    '.ok and .data.force_next_boot == true and
     .data.verification == "forced-unverified" and
     .data.reboot_required == true' >/dev/null
[ "$(cat ./build/test-vendor-config/vendor-import-force-next-boot)" = \
  "force-unverified-owner-local-import-v1" ]
[ "$(stat -c '%a' ./build/test-vendor-config/vendor-import-force-next-boot)" = 600 ]
curl -fsS "$URL/api/v1/system" | jq -e \
    '.ok and .data.ntp == true and .data.ntp_state == "synchronized" and
     .data.clock_source == "ntp" and .data.rtc_available == true and
     .data.rtc_persisted == true and .data.last_sync_epoch == 1700000000 and
     .data.ntp_servers == "time.cloudflare.com,time.nist.gov"' >/dev/null
echo 'linux unsupported: ok'
kill "$pid"
wait "$pid" 2>/dev/null || true
pid=0
if ./build/libreecho-web --backend mock --listen 0.0.0.0:18084 --web-root ./web >./build/test-insecure-lan.log 2>&1; then
    echo 'unauthenticated LAN bind was not refused' >&2
    exit 1
fi
printf '%s\n' 'test-token-0123456789abcdef' >./build/test-auth-token
chmod 600 ./build/test-auth-token
./build/libreecho-web --backend mock --config "$CFG" --web-root ./web --listen "127.0.0.1:$PORT" --auth-token-file ./build/test-auth-token --allowed-origin http://device.test >./build/test-auth.log 2>&1 &
pid=$!
sleep 1
curl -fsS "$URL/api/v1/config" | grep -q 'bearer-token'
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
code=$(curl -sS -o /tmp/le-auth.out -w '%{http_code}' "$URL/api/v1/status")
[ "$code" = 401 ]
curl -fsS "$URL/api/v1/status" -H 'Authorization: Bearer test-token-0123456789abcdef' >/dev/null
code=$(curl -sS -o /tmp/le-origin.out -w '%{http_code}' -X PUT "$URL/api/v1/network" -H 'Authorization: Bearer test-token-0123456789abcdef' -H "$CSRF" -H 'Origin: http://evil.test' -H 'Content-Type: application/json' --data '{"hostname":"blocked"}')
[ "$code" = 403 ]
curl -fsS -X PUT "$URL/api/v1/network" -H 'Authorization: Bearer test-token-0123456789abcdef' -H "$CSRF" -H 'Origin: http://device.test' -H 'Content-Type: application/json' --data '{"hostname":"allowed"}' >/dev/null
echo 'authentication and origin: ok'
echo 'all tests: ok'
