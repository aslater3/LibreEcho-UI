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
# An empty init field makes a custom service non-restartable, which is the
# state a service is in when nothing can be done for it but a reboot. Repeated
# failed passes must not repeat the warning and fill the device's fixed log
# ring. (This used to be waked's situation; waked is now restarted with micd
# as a group, so the case is driven by a custom service instead.)
unrestartable_log="$dir/waked.log"
( sleep 2; kill "$(cat "$dir/pid")" 2>/dev/null || true; rm -f "$sock" ) &
"$WD" --passes 6 --interval 1 --service "unrestartable:$sock:" >"$unrestartable_log" 2>&1
wait
warnings=$(grep -c 'watchdog: unrestartable is not answering (not restartable; a reboot is needed)' \
    "$unrestartable_log" || true)
if [ "$warnings" != "1" ]; then
    echo "FAIL: non-restartable failure was reported $warnings times"
    cat "$unrestartable_log"
    exit 1
fi
echo "  non-restartable failure reported once: ok"

# --- a group is restarted together, producer first ------------------------
# micd and waked are one unit: waked is micd's only microphone-stream
# consumer and exits when that stream ends, so restarting micd alone would
# take the wake word down until the next reboot. Killing the consumer must
# bring back both, and the producer must start first.
gdir="$dir/group"
mkdir -p "$gdir"
order="$gdir/order"
: > "$order"

for member in producer consumer; do
    cat > "$gdir/$member.init" <<EOF
#!/bin/sh
case "\$1" in
  start)
    echo "start $member" >> "$order"
    python3 "$dir/fake.py" "$gdir/$member.sock" "$gdir/$member.pid" &
    sleep 1
    ;;
  stop)
    echo "stop $member" >> "$order"
    [ -f "$gdir/$member.pid" ] && kill "\$(cat "$gdir/$member.pid")" 2>/dev/null
    rm -f "$gdir/$member.sock"
    ;;
esac
exit 0
EOF
    chmod +x "$gdir/$member.init"
    sh "$gdir/$member.init" start
done
sleep 1
: > "$order"

# kill only the consumer
( sleep 3
  [ -f "$gdir/consumer.pid" ] && kill "$(cat "$gdir/consumer.pid")" 2>/dev/null
  rm -f "$gdir/consumer.sock" ) &
"$WD" --passes 12 --interval 1 \
    --service "producer:$gdir/producer.sock:$gdir/producer.init:capture" \
    --service "consumer:$gdir/consumer.sock:$gdir/consumer.init:capture" \
    >/dev/null 2>&1
wait

if ! grep -q "start producer" "$order"; then
    echo "FAIL: the producer was not restarted when its consumer died"
    cat "$order"
    exit 1
fi
if ! grep -q "start consumer" "$order"; then
    echo "FAIL: the consumer was not restarted"
    cat "$order"
    exit 1
fi
echo "  group restarted together: ok"

# the producer must be started before the consumer, or the consumer has
# nothing to attach to and exits again immediately
if [ "$(grep -n 'start producer' "$order" | head -1 | cut -d: -f1)" -ge \
     "$(grep -n 'start consumer' "$order" | head -1 | cut -d: -f1)" ]; then
    echo "FAIL: the consumer started before its producer"
    cat "$order"
    exit 1
fi
echo "  producer started before consumer: ok"

# and it must be stopped last, so the consumer is gone before its stream is
if [ "$(grep -n 'stop consumer' "$order" | head -1 | cut -d: -f1)" -ge \
     "$(grep -n 'stop producer' "$order" | head -1 | cut -d: -f1)" ]; then
    echo "FAIL: the producer was stopped before its consumer"
    cat "$order"
    exit 1
fi
echo "  consumer stopped before producer: ok"

[ -f "$gdir/producer.pid" ] && kill "$(cat "$gdir/producer.pid")" 2>/dev/null
[ -f "$gdir/consumer.pid" ] && kill "$(cat "$gdir/consumer.pid")" 2>/dev/null

echo "watchdogd recovery: ok"
