#!/bin/sh
set -eu

test_dir=$(mktemp -d)
socket_path="$test_dir/led.sock"
log_path=./build/test-led-pattern-ownership.log
pid=0

cleanup() {
    if [ "$pid" -gt 1 ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rmdir "$test_dir" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

./build/libreecho-ledd --foreground --stub --socket "$socket_path" \
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

LIBREECHO_LED_TEST_SOCKET="$socket_path" python3 - <<'PY'
import json
import os
import socket

path = os.environ["LIBREECHO_LED_TEST_SOCKET"]
sequence = 0


def call(command, arguments=None):
    global sequence
    sequence += 1
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(1)
    client.connect(path)
    request = {
        "v": 1,
        "id": sequence,
        "cmd": command,
        "args": arguments or {},
    }
    client.sendall(
        json.dumps(request, separators=(",", ":")).encode("ascii") + b"\n"
    )
    response = b""
    while not response.endswith(b"\n"):
        response += client.recv(4096)
    client.close()
    parsed = json.loads(response)
    assert parsed["ok"], parsed
    return parsed.get("data", {})


assert call("status")["pattern_active"] is False
call(
    "pattern",
    {
        "name": "pulse",
        "r": 255,
        "g": 180,
        "b": 0,
        "brightness": 70,
        "repeats": 0,
        "owner": "bluetooth",
    },
)
assert call("status")["pattern_owner"] == "bluetooth"
call(
    "pattern",
    {
        "name": "pulse",
        "r": 0,
        "g": 255,
        "b": 0,
        "brightness": 55,
        "repeats": 0,
        "owner": "announcement",
    },
)
assert call("status")["pattern_owner"] == "announcement"
call("pattern", {"name": "stop", "owner": "announcement"})
restored = call("status")
assert restored["pattern"] == "pulse", restored
assert restored["pattern_owner"] == "bluetooth", restored
call("pattern", {"name": "stop", "owner": "bluetooth"})
assert call("status")["pattern_active"] is False
PY

echo "LED owner-scoped override and restore: ok"
