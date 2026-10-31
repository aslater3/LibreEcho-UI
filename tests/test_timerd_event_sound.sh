#!/bin/sh
# timerd rings with the owner's own sound when one is selected (#410), falls
# back to the built-in cue otherwise, and stops the owner's ring on dismiss.
# A stand-in audiod records every command; nothing touches ALSA.
set -eu

WD=./build/libreecho-timerd
[ -x "$WD" ] || { echo "timerd not built"; exit 1; }

dir=$(mktemp -d)
sock="$dir/timer.sock"
audio="$dir/audio.sock"
calls="$dir/calls"
mode="$dir/mode"
: > "$calls"

cleanup() {
    [ -f "$dir/timerd.pid" ] && kill "$(cat "$dir/timerd.pid")" 2>/dev/null || true
    [ -f "$dir/audio.pid" ] && kill "$(cat "$dir/audio.pid")" 2>/dev/null || true
    rm -rf "$dir"
}
trap cleanup EXIT INT TERM

python3 - "$audio" "$calls" "$mode" <<'PY' >"$dir/audio.log" 2>&1 &
import json, os, socket, sys
path, calls, mode = sys.argv[1:4]
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(path); server.listen(8); server.settimeout(60)
while True:
    try:
        conn, _ = server.accept()
    except socket.timeout:
        break
    conn.settimeout(2)
    data = b""
    try:
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                break
            data += chunk
            while b"\n" in data:
                line, data = data.split(b"\n", 1)
                req = json.loads(line)
                with open(calls, "a") as f:
                    f.write(req["cmd"] + " " + json.dumps(req.get("args", {}), sort_keys=True) + "\n")
                custom = os.path.exists(mode)
                if req["cmd"] == "event_sound":
                    out = {"custom": custom, "playing": custom}
                elif req["cmd"] == "event_sound_stop":
                    out = {"stopped": True}
                else:
                    out = {}
                conn.sendall((json.dumps({"v": 1, "id": req.get("id", 0), "ok": True, "data": out}) + "\n").encode())
    except (socket.timeout, ConnectionError):
        pass
    conn.close()
PY
echo $! > "$dir/audio.pid"
waited=0
while [ ! -S "$audio" ] && [ "$waited" -lt 50 ]; do sleep 0.1; waited=$((waited+1)); done
[ -S "$audio" ] || { echo "FAIL: stand-in audiod did not start"; cat "$dir/audio.log"; exit 1; }

call() {
    python3 - "$sock" "$1" "$2" <<'PY'
import json, socket, sys
sock, cmd, args = sys.argv[1], sys.argv[2], sys.argv[3]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5); s.connect(sock)
s.sendall((json.dumps({"v": 1, "id": 1, "cmd": cmd, "args": json.loads(args)}) + "\n").encode())
print(s.recv(65536).decode().strip())
s.close()
PY
}

"$WD" --foreground --socket "$sock" --state "$dir/timers" --audio-socket "$audio" >>"$dir/timerd.log" 2>&1 &
echo $! > "$dir/timerd.pid"
waited=0
while [ ! -S "$sock" ] && [ "$waited" -lt 50 ]; do sleep 0.1; waited=$((waited+1)); done
[ -S "$sock" ] || { echo "FAIL: timerd did not start"; cat "$dir/timerd.log"; exit 1; }

# --- no owner sound: the built-in two-tone cue rings ---------------------
call add '{"seconds":1,"label":"builtin"}' >/dev/null
sleep 2
grep -q '^event_sound {"event": "timer"}' "$calls" || { echo "FAIL: timerd did not ask for the owner's ring"; cat "$calls"; exit 1; }
grep -q '^cue ' "$calls" || { echo "FAIL: no built-in cue without an owner sound"; cat "$calls"; exit 1; }
call dismiss '{}' >/dev/null
sleep 1
if grep -q '^event_sound_stop' "$calls"; then echo "FAIL: built-in ring sent an owner-sound stop"; cat "$calls"; exit 1; fi
echo "  built-in cue rings when no owner sound is selected: ok"

# --- owner sound selected: it replaces the cue and stops on dismiss ------
: > "$calls"
: > "$mode"
call add '{"seconds":1,"label":"custom"}' >/dev/null
sleep 2
grep -q '^event_sound {"event": "timer"}' "$calls" || { echo "FAIL: owner ring not requested"; cat "$calls"; exit 1; }
if grep -q '^cue ' "$calls"; then echo "FAIL: built-in cue played over the owner's ring"; cat "$calls"; exit 1; fi
call dismiss '{}' >/dev/null
sleep 1
grep -q '^event_sound_stop {"event": "timer"}' "$calls" || { echo "FAIL: dismiss did not stop the owner's ring"; cat "$calls"; exit 1; }
echo "  owner ring replaces the cue and stops on dismiss: ok"
