#!/bin/sh
# Executes the shipped /etc/init.d lifecycle wrapper against the real
# libreecho-mdnsd supervisor.
#
# The chroot children are stubs, so this test proves lifecycle, locking,
# ownership and the shared-bus layout. It is not evidence that Avahi published
# a record on a device.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
SCRIPT=$ROOT/init/libreecho-mdnsd.init
umask 022
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

sh -n "$SCRIPT"

if grep -Fq 'killall' "$SCRIPT"; then
    echo "mdnsd init must not use a global process sweep" >&2
    exit 1
fi

cat >"$TMP/chroot" <<'EOF'
#!/bin/sh
# libreecho-mdnsd invokes: CHROOT chroot ROOT /usr/{bin,sbin}/daemon ...
[ "$1" = chroot ] || exit 40
root=$2
[ -d "$root/run/dbus" ] || exit 41
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
STATE_ROOT=$RUNDIR/mdns-state
BUS_DIR=$STATE_ROOT/dbus
mkdir -p "$PAYLOAD/etc/avahi/services" "$PAYLOAD/run" "$RUNDIR"
PIDFILE=$TMP/mdnsd.pid
LOGFILE=$TMP/mdnsd.log
SOCKET=$RUNDIR/mdns.sock
LOCKDIR=$TMP/lifecycle.lock
CONFIG=$TMP/web-config.json
DEFAULTS=$TMP/libreecho-wyomingd
WYOMING_SOURCE=$TMP/wyoming.service
MOUNT_LOG=$TMP/mount.log
cp "$ROOT/config/wyoming.service" "$WYOMING_SOURCE"
OWNER_UID=$(id -u)

# CI cannot issue real bind mounts. These stubs model mount presence in a small
# fixture file while preserving the wrapper's fail-closed decisions.
cat >"$TMP/mount" <<'EOF'
#!/bin/sh
[ "$1" = --bind ] || exit 2
printf '%s %s bind rw 0 0\n' "$2" "$3" >>"$MOUNT_LOG"
exit 0
EOF
cat >"$TMP/umount" <<'EOF'
#!/bin/sh
target=$1
[ -f "$MOUNT_LOG" ] || exit 0
awk -v target="$target" '$2 != target' "$MOUNT_LOG" >"$MOUNT_LOG.tmp"
mv "$MOUNT_LOG.tmp" "$MOUNT_LOG"
EOF
chmod 755 "$TMP/mount" "$TMP/umount"

# Override /proc/mounts-backed detection for the unprivileged host fixture by
# generating a test copy whose mount_present reads our fixture. Production
# still reads /proc/mounts.
TEST_SCRIPT=$TMP/libreecho-mdnsd.init
awk -v fixture="$MOUNT_LOG" '
  /^mount_present\(\) \{/ { in_mount=1; print; print "    target=$1"; print "    [ -f \"" fixture "\" ] || return 1"; print "    awk -v target=\"$target\" '\''$2 == target { found=1 } END { exit found ? 0 : 1 }'\'' \"" fixture "\" >/dev/null 2>&1"; print "}"; next }
  in_mount && /^\}/ { in_mount=0; next }
  in_mount { next }
  { print }
' "$SCRIPT" >"$TEST_SCRIPT"
chmod 755 "$TEST_SCRIPT"
SCRIPT=$TEST_SCRIPT

init() {
    DAEMON="$1" \
    PIDFILE="$PIDFILE" LOGFILE="$LOGFILE" ROOT="$PAYLOAD" SOCKET="$SOCKET" \
    STATE_ROOT="$STATE_ROOT" BUS_DIR="$BUS_DIR" LOCKDIR="$LOCKDIR" CONFIG="$CONFIG" \
    WYOMING_SERVICE_SOURCE="$WYOMING_SOURCE" WYOMING_DEFAULTS="$DEFAULTS" \
    MDNSD_OWNER_UID="$OWNER_UID" MOUNT="$TMP/mount" UMOUNT="$TMP/umount" \
    MOUNT_LOG="$MOUNT_LOG" \
    sh "$SCRIPT" "$2"
}

alive() {
    [ -s "$PIDFILE" ] && kill -0 "$(sed -n '1p' "$PIDFILE")" 2>/dev/null
}

launched=0
cleanup() {
    if [ "$launched" -eq 1 ]; then
        init "$TMP/mdnsd" stop >/dev/null 2>&1 || true
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

# 3. A non-private runtime root is refused.
mkdir -p "$TMP/shared/etc/avahi/services"
chmod 0777 "$TMP/shared"
if DAEMON="$TMP/mdnsd" PIDFILE="$PIDFILE" LOGFILE="$LOGFILE" \
   ROOT="$TMP/shared" SOCKET="$SOCKET" STATE_ROOT="$STATE_ROOT" BUS_DIR="$BUS_DIR" \
   LOCKDIR="$LOCKDIR" MDNSD_OWNER_UID="$OWNER_UID" MOUNT="$TMP/mount" \
   UMOUNT="$TMP/umount" MOUNT_LOG="$MOUNT_LOG" sh "$SCRIPT" start; then
    echo "start with a world-writable runtime root must fail" >&2
    exit 1
fi
rm -rf "$TMP/shared"

# 4. A normal start prepares the chroot system-bus path, binds the contract bus
#    directory into it, renders the Wyoming record and owns a private socket.
printf '%s\n' '{"integrations": 1}' >"$CONFIG"
printf '%s\n' 'PORT=12345' >"$DEFAULTS"
if ! init "$TMP/mdnsd" start; then
    echo "start failed" >&2
    cat "$LOGFILE" >&2 || true
    exit 1
fi
launched=1
alive || { echo "supervisor not running after start" >&2; exit 1; }
[ -d "$PAYLOAD/run/dbus" ] || { echo "chroot /run/dbus was not created" >&2; exit 1; }
[ -d "$PAYLOAD/run/avahi-daemon" ] || { echo "chroot Avahi run directory was not created" >&2; exit 1; }
awk -v source="$BUS_DIR" -v target="$PAYLOAD/run/dbus" \
    '$1 == source && $2 == target { found=1 } END { exit found ? 0 : 1 }' "$MOUNT_LOG" || {
        echo "shared bus was not bound into the chroot" >&2; exit 1;
    }
[ -S "$SOCKET" ] || { echo "supervisor socket missing" >&2; exit 1; }
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

# 6. Disabling Home Assistant removes the record without touching the socket.
printf '%s\n' '{"integrations": 0}' >"$CONFIG"
init "$TMP/mdnsd" restart >/dev/null
[ -S "$SOCKET" ] || { echo "restart lost the socket" >&2; exit 1; }
[ ! -e "$service" ] || { echo "Wyoming record survived a disable" >&2; exit 1; }

# 7. Stop terminates only this supervisor, clears runtime files and unmounts the
#    chroot bus view.
if ! init "$TMP/mdnsd" stop; then
    echo "stop failed" >&2
    exit 1
fi
launched=0
[ ! -s "$PIDFILE" ] || { echo "pidfile survived stop" >&2; exit 1; }
[ ! -e "$SOCKET" ] || { echo "socket survived stop" >&2; exit 1; }
if [ -s "$MOUNT_LOG" ] && grep -Fq " $PAYLOAD/run/dbus " "$MOUNT_LOG"; then
    echo "chroot bus bind survived stop" >&2
    exit 1
fi
if init "$TMP/mdnsd" status; then
    echo "status must report stopped after stop" >&2
    exit 1
fi

printf '%s\n' 'shared mDNS lifecycle wrapper: ok'
