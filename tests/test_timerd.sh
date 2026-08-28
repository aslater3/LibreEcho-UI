#!/bin/sh
# The timer daemon over its real socket.
#
# The scheduling rules are unit-tested; what this covers is the wiring the
# unit test cannot -- that the protocol works, that a due timer actually rings
# on the audio daemon, and that the schedule survives a restart.
set -eu

WD=./build/libreecho-timerd
AD=./build/libreecho-audiod
[ -x "$WD" ] || { echo "timerd not built"; exit 1; }
[ -x "$AD" ] || { echo "audiod not built"; exit 1; }

dir=$(mktemp -d)
sock="$dir/timer.sock"
audio="$dir/audio.sock"
state="$dir/timers"
bus="$dir/system.pcm"
: > "$bus"

cleanup() {
    [ -f "$dir/timerd.pid" ] && kill "$(cat "$dir/timerd.pid")" 2>/dev/null || true
    [ -f "$dir/audio.pid" ] && kill "$(cat "$dir/audio.pid")" 2>/dev/null || true
    rm -rf "$dir"
}
trap cleanup EXIT INT TERM

start_audio() {
    "$AD" --foreground --socket "$audio" --system-bus "$bus" \
        >>"$dir/audiod.log" 2>&1 &
    echo $! > "$dir/audio.pid"
    waited=0
    while [ ! -S "$audio" ] && [ "$waited" -lt 10 ]; do sleep 1; waited=$((waited+1)); done
    [ -S "$audio" ] || { echo "FAIL: audiod did not create its socket"; cat "$dir/audiod.log"; exit 1; }
}

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

start_audio
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

# --- malformed and oversized labels are refused --------------------------
long_label=$(printf '%048d' 0 | tr '0' x)
out=$(call add "{\"seconds\":600,\"label\":\"$long_label\"}")
case "$out" in *'"ok":false'*) ;; *) echo "FAIL: oversized label accepted: $out"; exit 1 ;; esac
out=$(call add '{"seconds":600,"label":"bad\u0001"}')
case "$out" in *'"ok":false'*) ;; *) echo "FAIL: malformed label accepted: $out"; exit 1 ;; esac
echo "  malformed and oversized labels refused: ok"

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
: > "$bus"
out=$(call add '{"seconds":1,"label":"short"}')
short_id=$(printf '%s' "$out" | sed -n 's/.*"id":\([0-9]*\).*/\1/p')
sleep 3
if [ "$(wc -c < "$bus")" -ne 48000 ]; then
    echo "FAIL: a due timer did not ring"
    cat "$dir/timerd.log"
    exit 1
fi
echo "  due timer rings: ok"

out=$(call status '{}')
case "$out" in *'"state":"ringing"'*) ;; *) echo "FAIL: not reported ringing: $out"; exit 1 ;; esac
echo "  ringing state reported: ok"

# --- it keeps ringing rather than chirping once ---------------------------
before=$(stat -c %Y "$bus")
sleep 3
after=$(stat -c %Y "$bus")
if [ "$after" -le "$before" ]; then
    echo "FAIL: the ring stopped on its own after one cue"
    exit 1
fi
echo "  ring repeats: ok"

# --- dismiss silences the ring and leaves the other timer alone -----------
out=$(call dismiss '{"id":"bad"}')
case "$out" in *'"ok":false'*) ;; *) echo "FAIL: malformed dismiss id accepted: $out"; exit 1 ;; esac
out=$(call status '{}')
case "$out" in *'"state":"ringing"'*) ;; *) echo "FAIL: malformed dismiss id dismissed the ring: $out"; exit 1 ;; esac
echo "  malformed dismiss id refused without dismissing: ok"
out=$(call dismiss '{}')
case "$out" in *'"dismissed":1'*) ;; *) echo "FAIL: dismiss did not stop one ring: $out"; exit 1 ;; esac
: > "$bus"
before_dismiss=$(stat -c %Y "$bus")
sleep 3
if [ "$(stat -c %Y "$bus")" -ne "$before_dismiss" ]; then
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

# --- cancel_all and next --------------------------------------------------
# Voice control needs "cancel everything" and "how long is left". They live in
# the daemon so a caller does not have to parse the list and re-derive what
# the schedule already knows.
out=$(call add '{"seconds":900,"label":"one"}')
out=$(call add '{"seconds":300,"label":"two"}')

out=$(call next '{}')
case "$out" in
    *'"count":2'*) ;;
    *) echo "FAIL: next did not count both timers: $out"; exit 1 ;;
esac
# The soonest one, not the first one added.
case "$out" in
    *'"label":"two"'*) ;;
    *) echo "FAIL: next reported the wrong timer: $out"; exit 1 ;;
esac
echo "  next reports the soonest timer: ok"

# A label-targeted cancellation removes only the requested timer.
out=$(call cancel '{"label":"two"}')
case "$out" in
    *'"ok":true'*) ;;
    *) echo "FAIL: label cancellation rejected: $out"; exit 1 ;;
esac
out=$(call status '{}')
case "$out" in
    *'"label":"two"'*)
        echo "FAIL: label cancellation removed the wrong timer: $out"; exit 1 ;;
    *'"label":"one"'*) ;;
    *) echo "FAIL: label cancellation removed the requested timer: $out"; exit 1 ;;
esac
echo "  label cancellation targets one timer: ok"

out=$(call cancel_all '{}')
case "$out" in
    *'"cancelled":1'*) ;;
    *) echo "FAIL: cancel_all did not cancel the remaining timer: $out"; exit 1 ;;
esac
out=$(call status '{}')
case "$out" in
    *'"timers":[]'*) ;;
    *) echo "FAIL: timers remain after cancel_all: $out"; exit 1 ;;
esac
echo "  cancel_all clears the schedule: ok"

# Nothing left: next must say so rather than reporting a stale timer.
out=$(call next '{}')
case "$out" in
    *'"count":0'*) ;;
    *) echo "FAIL: next after cancel_all: $out"; exit 1 ;;
esac
echo "  next reports an empty schedule: ok"

echo "timerd: ok"
