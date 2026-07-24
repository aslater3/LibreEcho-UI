#!/bin/sh
set -eu

test_dir=$(mktemp -d)
socket_path="$test_dir/led.sock"
log_path=./build/test-led-visualizer.log
ledd_bin=${LIBREECHO_TEST_LEDD:-./build/libreecho-ledd}
pid=0

cleanup() {
    if [ "$pid" -gt 1 ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rmdir "$test_dir" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

"$ledd_bin" --foreground --stub --socket "$socket_path" >"$log_path" 2>&1 &
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
import time

path = os.environ["LIBREECHO_LED_TEST_SOCKET"]
sequence = 0


def call(command, arguments=None, expect_ok=True):
    global sequence
    sequence += 1
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(1)
    client.connect(path)
    client.sendall(
        json.dumps(
            {"v": 1, "id": sequence, "cmd": command, "args": arguments or {}},
            separators=(",", ":"),
        ).encode("ascii")
        + b"\n"
    )
    response = b""
    while not response.endswith(b"\n"):
        response += client.recv(4096)
    client.close()
    parsed = json.loads(response)
    assert parsed["ok"] is expect_ok, parsed
    return parsed.get("data", {})


def frame(levels="20406080a0c0e0ffc0806040", brightness=72, owner="music"):
    return call(
        "visualizer",
        {
            "action": "frame",
            "levels": levels,
            "brightness": brightness,
            "owner": owner,
        },
    )


initial = call("status")
assert initial["visualizer_enabled"] is True
assert initial["visualizer_active"] is False
assert initial["visualizer_owner"] == ""
assert initial["visualizer_mood"] == "idle"
assert initial["visualizer_levels"] == [0] * 12
assert len(initial["pixels"]) == 12
steady_pixels = initial["pixels"]

call("set_visualizer_enabled", {"enabled": False})
disabled = call("status")
assert disabled["visualizer_enabled"] is False
frame()
ignored = call("status")
assert ignored["visualizer_active"] is False
assert ignored["visualizer_levels"] == [0] * 12
assert ignored["pixels"] == steady_pixels
call("set_visualizer_enabled", {"enabled": True})
assert call("status")["visualizer_enabled"] is True

for arguments in (
    {"action": "frame", "levels": "00", "brightness": 50, "owner": "music"},
    {
        "action": "frame",
        "levels": "20406080a0c0e0ffc080604g",
        "brightness": 50,
        "owner": "music",
    },
    {
        "action": "frame",
        "levels": "20406080a0c0e0ffc080604000",
        "brightness": 50,
        "owner": "music",
    },
    {
        "action": "frame",
        "levels": "20406080a0c0e0ffc0806040",
        "brightness": 101,
        "owner": "music",
    },
    {
        "action": "frame",
        "levels": "20406080a0c0e0ffc0806040",
        "brightness": 50,
        "owner": "bad owner",
    },
    {"action": "stop", "owner": ""},
    {"action": "warp", "owner": "music"},
):
    call("visualizer", arguments, expect_ok=False)

frame()
active = call("status")
assert active["visualizer_active"] is True
assert active["visualizer_owner"] == "music"
assert active["visualizer_mood"] in {
    "calm", "balanced", "energetic", "intense"
}
assert active["visualizer_levels"] == [
    0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0,
    0xE0, 0xFF, 0xC0, 0x80, 0x60, 0x40,
]
assert len({(p["r"], p["g"], p["b"]) for p in active["pixels"]}) > 3

# A low, steady spectrum gets the cool/calm palette. A sustained, dense
# spectrum gets the red/orange intense palette. These are acoustic moods,
# deliberately not assertions about a named genre.
call("visualizer", {"action": "stop", "owner": "music"})
frame("202020202020202020202020")
calm = call("status")
assert calm["visualizer_mood"] == "calm"
assert sum(p["b"] + p["g"] for p in calm["pixels"]) > \
       sum(p["r"] for p in calm["pixels"])
call("visualizer", {"action": "stop", "owner": "music"})
frame("f0f0f0f0f0f0f0f0f0f0f0f0")
intense = call("status")
assert intense["visualizer_mood"] == "intense"
assert sum(p["r"] for p in intense["pixels"]) > \
       sum(p["b"] for p in intense["pixels"]) * 2

call("visualizer", {"action": "stop", "owner": "other"})
assert call("status")["visualizer_active"] is True
call("visualizer", {"action": "stop", "owner": "music"})
stopped = call("status")
assert stopped["visualizer_active"] is False
assert stopped["visualizer_mood"] == "idle"
assert stopped["pixels"] == steady_pixels

frame()
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
overridden = call("status")
assert overridden["pattern_active"] is True
assert overridden["visualizer_active"] is True
assert len({(p["r"], p["g"], p["b"]) for p in overridden["pixels"]}) == 1

# Continuous music frames remain low priority, then resume when the owner
# overlay releases the ring.
frame("ffb080604020182030405060")
call("pattern", {"name": "stop", "owner": "bluetooth"})
resumed = call("status")
assert resumed["pattern_active"] is False
assert resumed["visualizer_active"] is True
assert len({(p["r"], p["g"], p["b"]) for p in resumed["pixels"]}) > 3

# The hardware test owns the output while it runs. Fresh music frames keep the
# visualizer alive underneath and it resumes after the test completes.
call("test")
testing = call("status")
assert testing["visualizer_active"] is True
assert len({(p["r"], p["g"], p["b"]) for p in testing["pixels"]}) == 1
for _ in range(12):
    frame()
    time.sleep(0.19)
after_test = call("status")
assert after_test["visualizer_active"] is True
assert len({(p["r"], p["g"], p["b"]) for p in after_test["pixels"]}) > 3

time.sleep(0.60)
expired = call("status")
assert expired["visualizer_active"] is False
assert expired["visualizer_owner"] == ""
assert expired["pixels"] == steady_pixels
PY

echo "LED audio visualizer protocol, priority and timeout: ok"
