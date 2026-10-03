#!/bin/sh
set -eu

test_dir=$(mktemp -d)
socket_path="$test_dir/led.sock"
ready_path="$test_dir/startup-ready"
log_path=./build/test-led-startup-animation.log
pid=0

cleanup() {
    if [ "$pid" -gt 1 ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -rf "$test_dir"
}
trap cleanup EXIT INT TERM

# The production init contract must opt into the startup animation, while the
# final web service must publish the readiness hand-off atomically.
grep -q -- '--startup-animation' init/libreecho-ledd.init
grep -q 'STARTUP_READY=.*startup-ready' init/libreecho-ledd.init
grep -q -- '--startup-ready $STARTUP_READY' init/libreecho-ledd.init
grep -q 'mark_startup_ready()' init/libreecho-web.init
grep -q 'tmp="$STARTUP_READY.tmp"' init/libreecho-web.init
grep -q 'bluetooth_integration_state()' init/libreecho-web.init
grep -q 'bluetooth_ready()' init/libreecho-web.init
grep -q 'case "$(bluetooth_integration_state)"' init/libreecho-web.init
grep -q 'airplay_integration_state()' init/libreecho-web.init
grep -q 'case "$(airplay_integration_state)"' init/libreecho-web.init
grep -q 'feature_policy()' init/libreecho-web.init
grep -q 'community-noncommercial' init/libreecho-web.init
grep -q '\[ -f "$BT_READY_PATH" \]' init/libreecho-web.init
if grep -q 'for socket in network audio mic led bluetooth airplay' init/libreecho-web.init; then
    echo 'unconditional Bluetooth readiness loop remains' >&2
    exit 1
fi
grep -q 'esphome_service_ready()' init/libreecho-web.init
! awk '/^esphome_service_ready\(\)/,/^}/' init/libreecho-web.init | grep -q 'redistributable'
grep -q 'ESPHOME_STATUS_FILE' init/libreecho-web.init
grep -q 'esphome_service_ready' init/libreecho-web.init

grep -q -- '--startup-animation' src/adapter/ledd.c
grep -q -- '--startup-ready' src/adapter/ledd.c
grep -q 'green startup animation started' src/adapter/ledd.c

grep -q 'startup_animation_active' src/adapter/ledd.c

./build/libreecho-ledd --foreground --stub --socket "$socket_path" \
    --startup-animation --startup-ready "$ready_path" \
    >"$log_path" 2>&1 &
pid=$!

i=0
while [ ! -S "$socket_path" ]; do
    i=$((i + 1))
    if [ "$i" -ge 30 ]; then
        cat "$log_path"
        exit 1
    fi
    sleep 0.1
done

LIBREECHO_LED_TEST_SOCKET="$socket_path" \
LIBREECHO_LED_TEST_READY="$ready_path" python3 - <<'PY'
import json
import os
import socket
import time

path = os.environ["LIBREECHO_LED_TEST_SOCKET"]
ready = os.environ["LIBREECHO_LED_TEST_READY"]
sequence = 0


def call(command):
    global sequence
    sequence += 1
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(1)
    client.connect(path)
    client.sendall(
        json.dumps(
            {"v": 1, "id": sequence, "cmd": command, "args": {}},
            separators=(",", ":"),
        ).encode("ascii") + b"\n"
    )
    response = b""
    while not response.endswith(b"\n"):
        response += client.recv(4096)
    client.close()
    parsed = json.loads(response)
    assert parsed["ok"], parsed
    return parsed["data"]


def assert_chase(status):
    assert status["startup_animation_active"] is True, status
    pixels = status["pixels"]
    assert len(pixels) == 12, status
    assert all(pixel["r"] == 0 and pixel["b"] == 0 for pixel in pixels), status
    # 0.14 routes the ring through the portable output pipeline, which applies
    # the gamma-2 perceptual transfer (ceil(v^2/255), led_output.c, asserted by
    # test_led_output_core). The logic value 255 stays 255; the trail value 64
    # drives 17, not 64. The status frame is the physical drive output.
    assert sum(pixel["g"] == 255 for pixel in pixels) == 1, status
    assert sum(pixel["g"] == 17 for pixel in pixels) == 11, status

assert_chase(call("status"))
time.sleep(0.12)
assert_chase(call("status"))
assert not os.path.exists(ready)

open(ready, "w", encoding="ascii").close()
for _ in range(20):
    status = call("status")
    if not status["startup_animation_active"]:
        break
    time.sleep(0.05)
else:
    raise AssertionError("startup animation did not stop after readiness")

status = call("status")
assert status["startup_animation_active"] is False, status
# #110 dark idle: the boot animation is an event that lights the ring, and with
# the default idle mode "off" the ring returns to the dark base once readiness
# stops it (ledd apply_base_state IDLE_MODE_OFF). It does not hold the boot
# colour. Proven by tests/test_led_daemon_core.c ("the ring must return to the
# dark base after an event").
assert status["pixels"] == [{"r": 0, "g": 0, "b": 0}] * 12, status
PY

echo "startup LED animation readiness hand-off: ok"
