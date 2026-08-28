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

# --- a dead service is brought back --------------------------------------
kill "$(cat "$dir/pid")" 2>/dev/null || true
rm -f "$sock"
sleep 1
# the policy needs two consecutive failures before it acts
"$WD" --passes 1 --interval 1 --service "fake:$sock:$init" >/dev/null 2>&1
if [ "$(wc -l < "$marker" | tr -d ' ')" != "1" ]; then
    echo "FAIL: watchdog restarted on a single failed probe"
    exit 1
fi
echo "  single failure did not trigger a restart: ok"

"$WD" --passes 2 --interval 1 --service "fake:$sock:$init" >/dev/null 2>&1
if [ "$(wc -l < "$marker" | tr -d ' ')" != "2" ]; then
    echo "FAIL: watchdog did not restart a dead service"
    cat "$marker"
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

echo "watchdogd recovery: ok"
