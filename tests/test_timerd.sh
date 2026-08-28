#!/bin/sh
# The timer daemon over its real socket.
#
# The scheduling rules are unit-tested; what this covers is the wiring the
# unit test cannot -- that the protocol works, that a due timer actually rings
# on the audio daemon, and that the schedule survives a restart.
set -eu

WD=./build/libreecho-timerd
[ -x "$WD" ] || { echo "timerd not built"; exit 1; }

dir=$(mktemp -d)
sock="$dir/timer.sock"
audio="$dir/audio.sock"
state="$dir/timers"
cues="$dir/cues"
: > "$cues"

cleanup() {
    [ -f "$dir/timerd.pid" ] && kill "$(cat "$dir/timerd.pid")" 2>/dev/null || true
    [ -f "$dir/audio.pid" ] && kill "$(cat "$dir/audio.pid")" 2>/dev/null || true
    rm -rf "$dir"
}
trap cleanup EXIT INT TERM

# A stand-in audio daemon that records the cues it is asked to play.
cat > "$dir/audio.py" <<'PY'
import os, socket, sys
path, log, pidfile = sys.argv[1], sys.argv[2], sys.argv[3]
try: os.unlink(path)
except FileNotFoundError: pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(path); s.listen(8)
open(pidfile, "w").write(str(os.getpid()))
while True:
    c, _ = s.accept()
    try:
        data = c.recv(4096).decode("utf-8", "replace")
        if '"cue"' in data:
            with open(log, "a") as f:
                f.write("cue\n")
        c.sendall(b'{"v":1,"id":1,"ok":true,"data":{},"error":null}\n')
    except Exception:
        pass
    c.close()
PY
python3 "$dir/audio.py" "$audio" "$cues" "$dir/audio.pid" &
sleep 1

# One request, one response.
call() {
    python3 - "$sock" "$1" "$2" <<'PY'
import json, socket, sys
sock, cmd, args = sys.argv[1], sys.argv[2], sys.argv[3]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5); s.connect(sock)
s.sendall((json.dumps({"v": 1, "id": 1, "cmd": cmd,
                       "args": json.loads(args)}) + "\n").encode())
print(s.recv(65536).decode().strip())
s.close()
PY
}

start_daemon() {
    "$WD" --foreground --socket "$sock" --state "$state" \
        --audio-socket "$audio" >>"$dir/timerd.log" 2>&1 &
    echo $! > "$dir/timerd.pid"
    waited=0
    while [ ! -S "$sock" ] && [ "$waited" -lt 10 ]; do sleep 1; waited=$((waited+1)); done
    [ -S "$sock" ] || { echo "FAIL: timerd did not create its socket"; cat "$dir/timerd.log"; exit 1; }
}
stop_daemon() {
    kill "$(cat "$dir/timerd.pid")" 2>/dev/null || true
    wait "$(cat "$dir/timerd.pid")" 2>/dev/null || true
    rm -f "$dir/timerd.pid"
}

start_daemon

# --- a timer can be added and listed --------------------------------------
out=$(call add '{"seconds":600,"label":"pasta"}')
case "$out" in *'"ok":true'*) ;; *) echo "FAIL: add rejected: $out"; exit 1 ;; esac
id=$(printf '%s' "$out" | sed -n 's/.*"id":\([0-9]*\).*/\1/p')
[ -n "$id" ] || { echo "FAIL: add returned no id: $out"; exit 1; }

out=$(call status '{}')
case "$out" in
    *'"label":"pasta"'*) ;;
    *) echo "FAIL: timer not listed: $out"; exit 1 ;;
esac
echo "  timer added and listed: ok"

# --- an out-of-range timer is refused, not clamped ------------------------
out=$(call add '{"seconds":0}')
case "$out" in *'"ok":false'*) ;; *) echo "FAIL: zero-second timer accepted: $out"; exit 1 ;; esac
echo "  out-of-range timer refused: ok"

# --- it survives a restart ------------------------------------------------
# A timer is a promise about the future; a daemon restart must not lose it.
stop_daemon
[ -s "$state" ] || { echo "FAIL: nothing persisted to $state"; exit 1; }
start_daemon
out=$(call status '{}')
case "$out" in
    *'"label":"pasta"'*) ;;
    *) echo "FAIL: timer did not survive a restart: $out"; exit 1 ;;
esac
echo "  timer survives a restart: ok"

# --- a due timer rings on the audio daemon --------------------------------
: > "$cues"
out=$(call add '{"seconds":1,"label":"short"}')
short_id=$(printf '%s' "$out" | sed -n 's/.*"id":\([0-9]*\).*/\1/p')
sleep 3
if [ ! -s "$cues" ]; then
    echo "FAIL: a due timer did not ring"
    cat "$dir/timerd.log"
    exit 1
fi
echo "  due timer rings: ok"

out=$(call status '{}')
case "$out" in *'"state":"ringing"'*) ;; *) echo "FAIL: not reported ringing: $out"; exit 1 ;; esac
echo "  ringing state reported: ok"

# --- it keeps ringing rather than chirping once ---------------------------
before=$(wc -l < "$cues")
sleep 3
after=$(wc -l < "$cues")
if [ "$after" -le "$before" ]; then
    echo "FAIL: the ring stopped on its own after one cue"
    exit 1
fi
echo "  ring repeats: ok"

# --- dismiss silences the ring and leaves the other timer alone -----------
out=$(call dismiss '{}')
case "$out" in *'"dismissed":1'*) ;; *) echo "FAIL: dismiss did not stop one ring: $out"; exit 1 ;; esac
: > "$cues"
sleep 3
if [ -s "$cues" ]; then
    echo "FAIL: still ringing after dismiss"
    exit 1
fi
out=$(call status '{}')
case "$out" in
    *'"label":"pasta"'*) ;;
    *) echo "FAIL: dismiss also removed the pending timer: $out"; exit 1 ;;
esac
echo "  dismiss silences the ring and spares pending timers: ok"

# --- cancel removes a pending timer ---------------------------------------
out=$(call cancel "{\"id\":$id}")
case "$out" in *'"ok":true'*) ;; *) echo "FAIL: cancel rejected: $out"; exit 1 ;; esac
out=$(call status '{}')
case "$out" in
    *'"label":"pasta"'*) echo "FAIL: cancelled timer still listed: $out"; exit 1 ;;
esac
out=$(call cancel "{\"id\":$id}")
case "$out" in *'"ok":false'*) ;; *) echo "FAIL: cancelling twice succeeded: $out"; exit 1 ;; esac
echo "  cancel removes a pending timer: ok"

# --- a ringing timer is not restored as ringing ---------------------------
# Coming back from a restart still ringing would be a surprise, not a service.
out=$(call add '{"seconds":1,"label":"restart"}')
sleep 3
stop_daemon
start_daemon
out=$(call status '{}')
case "$out" in
    *'"state":"ringing"'*) echo "FAIL: a ring survived a restart: $out"; exit 1 ;;
esac
echo "  a ring does not survive a restart: ok"

echo "timerd: ok"
