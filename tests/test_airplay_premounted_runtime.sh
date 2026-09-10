#!/bin/sh
# Regression cover for AirPlay runtime activation on a pre-mounted payload.
#
# The v2 feature transaction pre-mounts the airplay2 payload squashfs at the
# runtime root (candidate and committed activation).  The airplayd init must
# still create its writable runtime support mounts below that root: without
# them the isolated glibc runtime cannot start (D-Bus/Avahi/NQPTP), the
# reconcile reports a start failure, and AirPlay stays down -- which also
# blocks the OTA health confirm on candidate boots.
#
# mount_runtime and its helpers are evaluated straight out of the shipped
# init script, with `mount` and `grep` stubbed on PATH, so this exercises the
# text that actually ships without requiring root or real mounts.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
SCRIPT=${SCRIPT:-$ROOT/init/libreecho-airplayd.init}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

fails=0
pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; fails=$((fails + 1)); }
expect() { # name needle file
    if grep -q -- "$2" "$3" 2>/dev/null; then pass "$1"; else fail "$1 (missing: $2)"; fi
}
reject() { # name needle file
    if grep -q -- "$2" "$3" 2>/dev/null; then fail "$1 (unexpected: $2)"; else pass "$1"; fi
}

# ---------------------------------------------------------------- stubs
# `mount` records every invocation and succeeds.
cat >"$TMP/mount" <<'EOF'
#!/bin/sh
echo "mount $*" >>"$MOUNT_LOG"
exit 0
EOF
chmod 755 "$TMP/mount"

# `grep` answers the two /proc/mounts runtime checks from the case env.
# Anything else fails closed (nothing else is mounted).
cat >"$TMP/grep" <<'EOF'
#!/bin/sh
case "$*" in
    *led.sock*) exit 1 ;;
esac
case "$*" in
    *"$RUNTIME_ROOT/run "*) [ "${SUBMONTS_MOUNTED:-0}" = 1 ] && exit 0 || exit 1 ;;
    *"$RUNTIME_ROOT "*) [ "${ROOT_MOUNTED:-0}" = 1 ] && exit 0 || exit 1 ;;
esac
exit 1
EOF
chmod 755 "$TMP/grep"

# Pull the real helpers out of the shipped script (not a copy).
HELPERS=$(sed -n \
    -e '/^mount_led_socket()/,/^}/p' \
    -e '/^create_support_mounts()/,/^}/p' \
    -e '/^mount_runtime()/,/^}/p' \
    -e '/^prepare_avahi_runtime()/,/^}/p' \
    "$SCRIPT")
[ -n "$HELPERS" ] || { echo "FAIL: no helpers found in $SCRIPT"; exit 1; }

run_case() { # name root_mounted submounts_mounted
    name=$1
    case_root=$TMP/$name
    mkdir -p "$case_root/runtime" "$case_root/avahi" "$case_root/run" "$case_root/log"
    : >"$case_root/payload.squashfs"
    export RUNTIME_ROOT="$case_root/runtime"
    export PAYLOAD="$case_root/payload.squashfs"
    export AVAHI_CONFIG_SNAPSHOT="$case_root/avahi/daemon.conf"
    export AVAHI_SERVICES_SOURCE="$case_root/avahi/services-source"
    export MOUNT_LOG="$case_root/mount.log"
    export ROOT_MOUNTED=$2
    export SUBMONTS_MOUNTED=$3
    : >"$MOUNT_LOG"
    set +e
    (
        PATH="$TMP:$PATH"
        export PATH
        eval "$HELPERS"
        mount_runtime
    ) 2>"$case_root/log/stderr.log"
    rc=$?
    set -e
    [ "$rc" -eq 0 ] || { fail "$name: mount_runtime rc=$rc"; return 0; }
    pass "$name: mount_runtime rc=0"
}

echo "pre-mounted root without support mounts (the regression)"
run_case premounted-bare 1 0
expect "pre-mounted: tmpfs run mount created" "libreecho-airplay-run $TMP/premounted-bare/runtime/run" "$TMP/premounted-bare/mount.log"
expect "pre-mounted: tmpfs var mount created" "libreecho-airplay-var" "$TMP/premounted-bare/mount.log"
expect "pre-mounted: dev bind created" "mount --bind /dev $TMP/premounted-bare/runtime/dev" "$TMP/premounted-bare/mount.log"
expect "pre-mounted: audio bind created" "mount --bind /run/libreecho-audio" "$TMP/premounted-bare/mount.log"
reject "pre-mounted: payload squashfs not re-mounted" "-t squashfs" "$TMP/premounted-bare/mount.log"

echo "pre-mounted root with support mounts (idempotence)"
run_case premounted-complete 1 1
reject "complete: no support mounts re-created" "libreecho-airplay-run" "$TMP/premounted-complete/mount.log"
reject "complete: payload squashfs not re-mounted" "-t squashfs" "$TMP/premounted-complete/mount.log"

echo "bare root (fresh mount path still works)"
run_case fresh 0 0
expect "fresh: payload squashfs mounted" "-t squashfs" "$TMP/fresh/mount.log"
expect "fresh: tmpfs run mount created" "libreecho-airplay-run" "$TMP/fresh/mount.log"

echo
if [ "$fails" -eq 0 ]; then
    printf '%s\n' 'AirPlay pre-mounted runtime activation: ok'
else
    printf '%s\n' "AirPlay pre-mounted runtime activation: $fails failure(s)" >&2
    exit 1
fi
