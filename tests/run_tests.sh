#!/bin/sh
set -eu
PORT=${LIBREECHO_TEST_PORT:-18082}
URL="http://127.0.0.1:$PORT"
CFG=./build/test-suite-config.json
rm -f "$CFG" "$CFG.bak" "$CFG.tmp"
cc -D_POSIX_C_SOURCE=200809L -std=c99 -Isrc tests/test_unit.c src/json.c src/config_store.c -o build/test-unit
./build/test-unit
grep -q '"SAVE_CONFIG\\n"' src/adapter/networkd.c
sh tests/test_led_pattern_ownership.sh
sh tests/test_led_visualizer.sh
sh tests/test_airplay_led_bridge.sh
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
./build/libreecho-web --backend mock --config "$CFG" --mock-config ./config/mock-state.json --web-root ./web --listen "127.0.0.1:$PORT" --seed 42 --dev-controls >./build/test-server.log 2>&1 &
pid=$!
cleanup(){ if [ "${pid:-0}" -gt 1 ]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi; }
trap cleanup EXIT INT TERM
i=0
while ! curl -fsS "$URL/api/v1/status" >/dev/null 2>&1; do i=$((i+1)); [ "$i" -lt 30 ] || { cat ./build/test-server.log; exit 1; }; sleep 0.1; done
LIBREECHO_TEST_URL="$URL" sh tests/test_api.sh
LIBREECHO_TEST_URL="$URL" LIBREECHO_TEST_CONFIG="$CFG" sh tests/test_config.sh
LIBREECHO_TEST_URL="$URL" sh tests/test_mock_behaviour.sh
LIBREECHO_TEST_URL="$URL" sh tests/test_limits.sh
sh tests/test_memory.sh "$pid"
kill "$pid"
wait "$pid" 2>/dev/null || true
pid=0
./tools/create-user.sh test-user test-password-123 >./build/test-users
chmod 600 ./build/test-users
./build/libreecho-web --backend mock --config "$CFG" --web-root ./web --listen "127.0.0.1:$PORT" --seed 42 --users-file ./build/test-users >./build/test-users.log 2>&1 &
pid=$!
sleep 1
LIBREECHO_TEST_URL="$URL" LIBREECHO_TEST_USERS=./build/test-users sh tests/test_auth.sh
kill "$pid"
wait "$pid" 2>/dev/null || true
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
    '.data.local_only == true and .data.log_retention_hours == 168' >/dev/null
curl -fsS "$URL/api/v1/integrations" | jq -e \
    '.data.items[] | select(.id == "home-assistant") | .enabled == true' \
    >/dev/null
curl -fsS "$URL/api/v1/config" | grep -q '"setup_completed":true'
curl -fsS "$URL/" | grep -q 'LibreEcho Control Centre'
echo 'persistence: ok'
kill "$pid"
wait "$pid" 2>/dev/null || true
pid=0
./build/libreecho-web --backend linux --config "$CFG" --web-root ./web --listen "127.0.0.1:$PORT" >./build/test-linux.log 2>&1 &
pid=$!
sleep 1
code=$(curl -sS -o /tmp/le-linux-audio.out -w '%{http_code}' "$URL/api/v1/audio")
[ "$code" = 501 ]
grep -q 'not_supported' /tmp/le-linux-audio.out
code=$(curl -sS -o /tmp/le-linux-config.out -w '%{http_code}' "$URL/api/v1/config/export")
[ "$code" = 200 ]
jq -e '.ok == true and .data.partial == true and (.data.unsupported | index("wake_word")) != null' /tmp/le-linux-config.out >/dev/null
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
