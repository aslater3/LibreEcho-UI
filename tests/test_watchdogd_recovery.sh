#!/bin/sh
# The watchdog against a real socket: it must leave a healthy service alone,
# and bring back one that has stopped answering.
#
# The policy is unit-tested separately; what this covers is the wiring the unit
# test cannot -- that probing a live socket works, that the restart actually
# runs the init script, and that a healthy service is not disturbed.
set -eu

dir=$(mktemp -d)
sock="$dir/fake.sock"
init="$dir/fake.init"
marker="$dir/restarts"
: > "$marker"

cleanup() {
    [ -f "$dir/pid" ] && kill "$(cat "$dir/pid")" 2>/dev/null || true
    rm -rf "$dir"
}
trap cleanup EXIT INT TERM

# A minimal service: answers the adapter "status" call and nothing else.
cat > "$dir/fake.py" <<'PY'
import os, socket, sys
path = sys.argv[1]
try: os.unlink(path)
except FileNotFoundError: pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(path); s.listen(4)
open(sys.argv[2], "w").write(str(os.getpid()))
while True:
    c, _ = s.accept()
    try:
        c.recv(4096)
        c.sendall(b'{"v":1,"id":1,"ok":true,"data":{},"error":null}\n')
    except Exception:
        pass
    c.close()
PY

# The "init script": records that it was asked to start, and starts the service.
cat > "$init" <<EOF
#!/bin/sh
case "\$1" in
  start)
    echo start >> "$marker"
    python3 "$dir/fake.py" "$sock" "$dir/pid" &
    sleep 1
    ;;
  stop)
    [ -f "$dir/pid" ] && kill "\$(cat "$dir/pid")" 2>/dev/null || true
    ;;
esac
exit 0
EOF
chmod +x "$init"

WD=./build/libreecho-watchdogd
[ -x "$WD" ] || { echo "watchdogd not built"; exit 1; }

# --- a healthy service is left alone -------------------------------------
sh "$init" start
sleep 1
"$WD" --passes 3 --interval 1 --service "fake:$sock:$init" >/dev/null 2>&1
if [ "$(wc -l < "$marker" | tr -d ' ')" != "1" ]; then
    echo "FAIL: watchdog restarted a healthy service"
    exit 1
fi
echo "  healthy service left alone: ok"

# --- a service that was never up is not started --------------------------
# Supervision latches on the first healthy probe, so a daemon that has never
# answered is left alone: it is disabled or not installed, and starting
# something the owner turned off is not recovery.
absent="$dir/absent"
: > "$absent.marker"
cat > "$absent" <<EOF
#!/bin/sh
echo start >> "$absent.marker"
exit 0
EOF
chmod +x "$absent"
"$WD" --passes 4 --interval 1 --service "gone:$dir/nothing.sock:$absent" \
    >/dev/null 2>&1
if [ -s "$absent.marker" ]; then
    echo "FAIL: watchdog started a service that was never running"
    exit 1
fi
echo "  service that was never up left alone: ok"

# --- a service that dies mid-run is brought back -------------------------
# Killing it while one watchdog process is running is the real sequence: the
# watchdog must see it healthy, then see it go, then restart it. Separate
# invocations cannot test this -- failure counting and the supervision latch
# are per-process state.
( sleep 3; kill "$(cat "$dir/pid")" 2>/dev/null; rm -f "$sock" ) &
"$WD" --passes 12 --interval 1 --service "fake:$sock:$init" >/dev/null 2>&1
wait

if [ "$(wc -l < "$marker" | tr -d ' ')" -lt "2" ]; then
    echo "FAIL: watchdog did not restart a service that died"
    exit 1
fi
echo "  dead service restarted: ok"

# --- and it is genuinely answering again ---------------------------------
sleep 1
if [ ! -S "$sock" ]; then
    echo "FAIL: socket did not come back after the restart"
    exit 1
fi
echo "  service answering again: ok"

# --- restarted once, not repeatedly --------------------------------------
# The service came back, so the remaining passes must leave it alone.
if [ "$(wc -l < "$marker" | tr -d ' ')" -gt "2" ]; then
    echo "FAIL: watchdog restarted the service more than once"
    cat "$marker"
    exit 1
fi
echo "  restarted once, not repeatedly: ok"

# --- a non-restartable failure is reported once --------------------------
# Waked has no control socket in production, but keep the socket probe here so
# this drives the same health transition as the real service. An empty init
# field makes the custom service non-restartable; repeated failed passes must
# not repeat the warning and fill the device's fixed log ring.
waked_log="$dir/waked.log"
( sleep 2; kill "$(cat "$dir/pid")" 2>/dev/null || true; rm -f "$sock" ) &
"$WD" --passes 6 --interval 1 --service "waked:$sock:" >"$waked_log" 2>&1
wait
warnings=$(grep -c 'watchdog: waked is not answering (not restartable; a reboot is needed)' \
    "$waked_log" || true)
if [ "$warnings" != "1" ]; then
    echo "FAIL: non-restartable failure was reported $warnings times"
    cat "$waked_log"
    exit 1
fi
echo "  non-restartable failure reported once: ok"

echo "watchdogd recovery: ok"
