#!/bin/sh
# Executes the shipped /etc/init.d lifecycle wrapper against the real
# libreecho-mdnsd supervisor.
#
# The chroot children are stubs: D-Bus exposes the same Unix socket the real
# packaged runtime creates and Avahi stays alive. This proves lifecycle,
# locking, machine-id preparation and readiness semantics without claiming
# that DNS-SD was published on a device.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
SCRIPT=$ROOT/init/libreecho-mdnsd.init
# A service wrapper must never create a group-writable directory, so the
# fixture uses the same umask an init script runs with.
umask 022
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

sh -n "$SCRIPT"

# The wrapper may only ever terminate the supervisor it owns; a global sweep
# would take unrelated local daemons down with it.
if grep -Fq 'killall' "$SCRIPT"; then
    echo "mdnsd init must not use a global process sweep" >&2
    exit 1
fi

cat >"$TMP/chroot" <<'EOF'
#!/bin/sh
root=$2
program=$3
if [ "$program" = /usr/bin/dbus-daemon ]; then
    exec python3 - "$root/run/dbus/system_bus_socket" <<'PY'
import os
import signal
import socket
import sys
import time

path = sys.argv[1]
os.makedirs(os.path.dirname(path), exist_ok=True)
try:
    os.unlink(path)
except FileNotFoundError:
    pass
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(path)
server.listen(1)

def stop(*_args):
    server.close()
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    raise SystemExit(0)

signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
while True:
    time.sleep(0.05)
PY
fi
trap 'exit 0' TERM INT
trap : HUP
while :; do sleep 0.05; done
EOF
chmod 755 "$TMP/chroot"
cc -std=c99 -Wall -Wextra -Werror \
    -DMDNS_CHROOT="\"$TMP/chroot\"" \
    -DMDNS_OWNER_EXE="\"$TMP/libreecho-wyomingd\"" \
    "$ROOT/src/adapter/mdnsd.c" "$ROOT/src/adapter/mdns_lease.c" -o "$TMP/mdnsd"

PAYLOAD=$TMP/payload
RUNDIR=$TMP/run
mkdir -p "$PAYLOAD/etc/avahi/services" "$PAYLOAD/run" "$RUNDIR"
PIDFILE=$TMP/mdnsd.pid
LOGFILE=$TMP/mdnsd.log
SOCKET=$RUNDIR/mdns.sock
LOCKDIR=$TMP/lifecycle.lock
CONFIG=$TMP/web-config.json
DEFAULTS=$TMP/libreecho-wyomingd
WYOMING_SOURCE=$TMP/wyoming.service
UUID_SOURCE=$TMP/random-uuid
cp "$ROOT/config/wyoming.service" "$WYOMING_SOURCE"
printf '%s\n' '01234567-89ab-cdef-0123-456789abcdef' >"$UUID_SOURCE"

# The wrapper requires the socket and runtime directories to be owned by the
# account it runs as. Production uses root (the default); the host test uses
# the invoking uid so the same ownership check is actually exercised.
OWNER_UID=$(id -u)

init() {
    DAEMON="$1" \
    PIDFILE="$PIDFILE" LOGFILE="$LOGFILE" ROOT="$PAYLOAD" SOCKET="$SOCKET" \
    LOCKDIR="$LOCKDIR" CONFIG="$CONFIG" MACHINE_ID_SOURCE="$UUID_SOURCE" \
    WYOMING_SERVICE_SOURCE="$WYOMING_SOURCE" WYOMING_DEFAULTS="$DEFAULTS" \
    MDNSD_OWNER_UID="$OWNER_UID" \
    sh "$SCRIPT" "$2"
}

alive() {
    [ -s "$PIDFILE" ] && kill -0 "$(sed -n '1p' "$PIDFILE")" 2>/dev/null
}

launched=0
cleanup() {
    if [ "$launched" -eq 1 ]; then
        DAEMON="$TMP/mdnsd" PIDFILE="$PIDFILE" LOGFILE="$LOGFILE" \
        ROOT="$PAYLOAD" SOCKET="$SOCKET" LOCKDIR="$LOCKDIR" \
        MACHINE_ID_SOURCE="$UUID_SOURCE" MDNSD_OWNER_UID="$OWNER_UID" \
        sh "$SCRIPT" stop >/dev/null 2>&1 || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

# 1. Fail closed when the supervisor is not installed.
if init "$TMP/absent-mdnsd" start; then
    echo "start with a missing supervisor must fail" >&2
    exit 1
fi
[ ! -e "$SOCKET" ]

# 2. A held lifecycle lock refuses the operation instead of racing it.
mkdir "$LOCKDIR"
printf '%s\n' "$$" >"$LOCKDIR/pid"
if init "$TMP/mdnsd" start; then
    echo "start while another lifecycle operation holds the lock must fail" >&2
    exit 1
fi
[ ! -e "$SOCKET" ] || { echo "daemon started while locked" >&2; exit 1; }
rm -rf "$LOCKDIR"

# 3. A non-private (writable by others) runtime root is refused.
mkdir -p "$TMP/shared/etc/avahi/services"
chmod 0777 "$TMP/shared"
if DAEMON="$TMP/mdnsd" PIDFILE="$PIDFILE" LOGFILE="$LOGFILE" \
   ROOT="$TMP/shared" SOCKET="$SOCKET" LOCKDIR="$LOCKDIR" \
   MACHINE_ID_SOURCE="$UUID_SOURCE" MDNSD_OWNER_UID="$OWNER_UID" sh "$SCRIPT" start; then
    echo "start with a world-writable runtime root must fail" >&2
    exit 1
fi
rm -rf "$TMP/shared"

# 4. A normal start creates the D-Bus machine id, waits for the real child
#    readiness contract, renders the Wyoming record and reports running.
printf '%s\n' '{"integrations": 1}' >"$CONFIG"
printf '%s\n' 'PORT=12345' >"$DEFAULTS"
if ! init "$TMP/mdnsd" start; then
    echo "start failed" >&2
    cat "$LOGFILE" >&2 || true
    exit 1
fi
launched=1
alive || { echo "supervisor not running after start" >&2; exit 1; }
[ -S "$SOCKET" ] || { echo "supervisor socket missing" >&2; exit 1; }
[ -S "$PAYLOAD/run/dbus/system_bus_socket" ] || { echo "D-Bus socket missing" >&2; exit 1; }
MACHINE_ID=$PAYLOAD/var/lib/dbus/machine-id
[ -f "$MACHINE_ID" ] || { echo "D-Bus machine id missing" >&2; exit 1; }
[ "$(sed -n '1p' "$MACHINE_ID")" = 0123456789abcdef0123456789abcdef ] || {
    echo "D-Bus machine id was not derived from the runtime UUID" >&2
    exit 1
}
[ "$(stat -c %u "$SOCKET")" = "$OWNER_UID" ] || { echo "socket not owned by the service account" >&2; exit 1; }
case "$(stat -c %a "$SOCKET")" in
    600|700) ;;
    *) echo "socket is not private: mode $(stat -c %a "$SOCKET")" >&2; exit 1 ;;
esac
service=$PAYLOAD/etc/avahi/services/wyoming.service
[ -f "$service" ] || { echo "Wyoming service definition not rendered" >&2; exit 1; }
grep -Fq '<port>12345</port>' "$service"
[ "$(init "$TMP/mdnsd" status)" = "running" ]

# 5. A second start is idempotent and does not add a second supervisor.
first=$(sed -n '1p' "$PIDFILE")
init "$TMP/mdnsd" start >/dev/null
[ "$(sed -n '1p' "$PIDFILE")" = "$first" ]

# 6. Disabling Home Assistant removes the record without touching readiness.
printf '%s\n' '{"integrations": 0}' >"$CONFIG"
init "$TMP/mdnsd" restart >/dev/null
[ -S "$SOCKET" ] || { echo "restart lost the socket" >&2; exit 1; }
[ -S "$PAYLOAD/run/dbus/system_bus_socket" ] || { echo "restart lost the D-Bus socket" >&2; exit 1; }
[ ! -e "$service" ] || { echo "Wyoming record survived a disable" >&2; exit 1; }

# 7. Status must fail if the D-Bus readiness socket disappears even while the
#    supervisor process and its control socket are still alive.
rm -f "$PAYLOAD/run/dbus/system_bus_socket"
if init "$TMP/mdnsd" status; then
    echo "status reported running without the D-Bus socket" >&2
    exit 1
fi

# 8. Stop terminates only this supervisor and clears its runtime files.
if ! init "$TMP/mdnsd" stop; then
    echo "stop failed" >&2
    exit 1
fi
launched=0
[ ! -s "$PIDFILE" ] || { echo "pidfile survived stop" >&2; exit 1; }
[ ! -e "$SOCKET" ] || { echo "socket survived stop" >&2; exit 1; }
if init "$TMP/mdnsd" status; then
    echo "status must report stopped after stop" >&2
    exit 1
fi

printf '%s\n' 'shared mDNS lifecycle wrapper: ok'
