#!/bin/sh
set -eu

sh -n init/libreecho-buttond.init
make build/libreecho-buttond >/dev/null

grep -q 'init/libreecho-buttond.init' Makefile
grep -q 'refresh_audio(ctx)' src/adapter/buttond.c
grep -q 'POLLHUP | POLLERR | POLLNVAL' src/adapter/buttond.c
grep -q 'rescan_requested' src/adapter/buttond.c

python3 - <<'PY'
from pathlib import Path

source = Path('src/adapter/buttond.c').read_text()
assert 'action_capable' in source
assert 'TEST_BIT(KEY_HELP, key_bits)' in source
assert 'action=%d' in source
assert 'json_get_int(buffer, "button_action_brightness", &value) > 0' in source
assert 'json_get_int(buffer, "button_mute_brightness", &value) > 0' in source
assert 'json_get_string(buffer, "button_action_sounds", value_text,\n                        sizeof(value_text)) == 1' in source
sample = source[source.index('static void play_sample'):source.index('#define CUE_LOW_HZ')]
assert 'ctx->tones' not in sample
PY

test_dir=$(mktemp -d)
socket_path="$test_dir/led.sock"
log_path=./build/test-buttond-led.log
pid=0
cleanup() {
    if [ "$pid" -gt 1 ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rmdir "$test_dir" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

./build/libreecho-ledd --foreground --stub --socket "$socket_path" >"$log_path" 2>&1 &
pid=$!
i=0
while [ ! -S "$socket_path" ]; do
    i=$((i + 1))
    [ "$i" -lt 30 ] || { cat "$log_path"; exit 1; }
    sleep 0.1
done

LIBREECHO_LED_TEST_SOCKET="$socket_path" python3 - <<'PY'
import json
import os
import socket

path = os.environ["LIBREECHO_LED_TEST_SOCKET"]

def call(args):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(1)
    client.connect(path)
    client.sendall((json.dumps({"v": 1, "id": 1, "cmd": "meter", "args": args}, separators=(",", ":")) + "\n").encode())
    response = b""
    while not response.endswith(b"\n"):
        response += client.recv(4096)
    client.close()
    return json.loads(response)

ok = call({"value": 75, "r": 255, "g": 140, "b": 0, "brightness": 70, "hold_ms": 1000, "owner": "buttons"})
assert ok["ok"] is True, ok
stopped = call({"action": "stop", "owner": "buttons"})
assert stopped["ok"] is True, stopped
bad = call({"value": 50, "r": 255, "g": 0, "brightness": 70, "owner": "buttons"})
assert bad["ok"] is False, bad
PY

echo "button daemon install, refresh, disconnect, and meter contracts: ok"
