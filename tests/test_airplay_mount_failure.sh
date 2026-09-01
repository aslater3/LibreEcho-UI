#!/bin/sh
set -eu

SCRIPT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)/init/libreecho-airplayd.init
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

run_case() {
    name=$1
    payload=$2
    mount_mode=$3
    case_root=$TMP/$name
    mkdir -p "$case_root/bin" "$case_root/run" "$case_root/log"
    daemon_marker=$case_root/daemon-launched
    daemon=$case_root/daemon.sh
    cat >"$daemon" <<EOF
#!/bin/sh
: >"$daemon_marker"
exit 0
EOF
    chmod 755 "$daemon"
    if [ "$mount_mode" = fail ]; then
        cat >"$case_root/bin/mount" <<'EOF'
#!/bin/sh
exit 1
EOF
        chmod 755 "$case_root/bin/mount"
    fi
    if [ "$payload" = present ]; then
        : >"$case_root/payload.squashfs"
    fi
    set +e
    PATH="$case_root/bin:$PATH" \
    DAEMON="$daemon" \
    PAYLOAD="$case_root/payload.squashfs" \
    RUNTIME_ROOT="$case_root/runtime" \
    PIDFILE="$case_root/service.pid" \
    LOGFILE="$case_root/log/airplay.log" \
    SOCKET="$case_root/run/airplay.sock" \
    CONFIG="$case_root/config.json" \
    AVAHI_SERVICES_SOURCE="$case_root/avahi-services" \
    AVAHI_CONFIG_SNAPSHOT="$case_root/avahi.conf" \
    sh "$SCRIPT" start
    rc=$?
    set -e
    [ "$rc" -ne 0 ] || {
        echo "$name: expected start failure, got rc=$rc" >&2
        exit 1
    }
    [ ! -e "$daemon_marker" ] || {
        echo "$name: daemon was launched after startup failure" >&2
        exit 1
    }
    echo "$name: rc=$rc and daemon not launched"
}

run_case payload-absent absent none
run_case squashfs-mount-fails present fail
printf '%s\n' 'AirPlay mount failure behavior: ok'
