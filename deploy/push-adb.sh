#!/bin/sh
# Deploy the LibreEcho release to a rooted device over ADB.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(dirname "$SCRIPT_DIR")
ADB=${ADB:-adb}
ADB_SERIAL=${ADB_SERIAL:-}
RESTART=0
REMOTE_SBIN=/usr/local/sbin
REMOTE_WEB=/usr/local/share/libreecho/web
REMOTE_CONFIG=/etc/libreecho/web-config.json

usage() {
    echo "Usage: $0 [--restart]"
    echo "  --restart  restart libreecho-web after deployment"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --restart) RESTART=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

fail() {
    echo "error: $*" >&2
    exit 1
}

if ! command -v "$ADB" >/dev/null 2>&1; then
    fail "adb not found: $ADB"
fi

adb_cmd() {
    if [ -n "$ADB_SERIAL" ]; then
        "$ADB" -s "$ADB_SERIAL" "$@"
    else
        "$ADB" "$@"
    fi
}

if ! devices_output=$(adb_cmd devices 2>&1); then
    echo "$devices_output" >&2
    fail "adb devices failed"
fi

device_count=$(printf '%s\n' "$devices_output" | awk 'NR > 1 && $2 == "device" { count++ } END { print count + 0 }')
[ "$device_count" -gt 0 ] || {
    echo "$devices_output" >&2
    fail "no usable ADB device found"
}
if [ -z "$ADB_SERIAL" ] && [ "$device_count" -gt 1 ]; then
    fail "multiple ADB devices found; set ADB_SERIAL to select one"
fi

for binary in libreecho-web libreecho-networkd libreecho-audiod libreecho-ledd; do
    [ -f "$ROOT/build/$binary" ] || fail "missing binary: build/$binary"
done
[ -d "$ROOT/web" ] || fail "missing web directory"
[ -f "$ROOT/config/defaults.json" ] || fail "missing config/defaults.json"
[ -f "$ROOT/build/source-provenance.json" ] || fail "missing build/source-provenance.json; run make release"
for init_script in \
    libreecho-web.init \
    libreecho-networkd.init \
    libreecho-audiod.init \
    libreecho-ledd.init
do
    [ -f "$ROOT/init/$init_script" ] || fail "missing init script: init/$init_script"
done

adb_cmd shell "mkdir -p $REMOTE_SBIN $REMOTE_WEB /etc/libreecho /etc/init.d" \
    || fail "could not create remote directories"

for binary in libreecho-web libreecho-networkd libreecho-audiod libreecho-ledd; do
    echo "Pushing $binary..."
    adb_cmd push "$ROOT/build/$binary" "$REMOTE_SBIN/$binary" \
        || fail "could not push $binary"
done

echo "Pushing web assets..."
adb_cmd push "$ROOT/web/." "$REMOTE_WEB/" \
    || fail "could not push web assets"

echo "Pushing source provenance..."
adb_cmd push "$ROOT/build/source-provenance.json" "/usr/local/share/libreecho/source-provenance.json" \
    || fail "could not push source provenance"

echo "Pushing configuration..."
adb_cmd push "$ROOT/config/defaults.json" "$REMOTE_CONFIG" \
    || fail "could not push configuration"

for init_script in \
    libreecho-web.init \
    libreecho-networkd.init \
    libreecho-audiod.init \
    libreecho-ledd.init
do
    echo "Pushing init/$init_script..."
    adb_cmd push "$ROOT/init/$init_script" "/etc/init.d/$init_script" \
        || fail "could not push $init_script"
done

adb_cmd shell "chmod 0755 \
    $REMOTE_SBIN/libreecho-web \
    $REMOTE_SBIN/libreecho-networkd \
    $REMOTE_SBIN/libreecho-audiod \
    $REMOTE_SBIN/libreecho-ledd \
    /etc/init.d/libreecho-web.init \
    /etc/init.d/libreecho-networkd.init \
    /etc/init.d/libreecho-audiod.init \
    /etc/init.d/libreecho-ledd.init" \
    || fail "could not set remote executable permissions"

if [ "$RESTART" -eq 1 ]; then
    echo "Restarting libreecho-web..."
    adb_cmd shell "/etc/init.d/libreecho-web.init restart" \
        || fail "could not restart libreecho-web"
fi

echo
echo "LibreEcho deployment completed successfully."
echo "  Binaries: $REMOTE_SBIN/"
echo "  Web UI:   $REMOTE_WEB/"
echo "  Access:   ${LIBREECHO_URL:-http://127.0.0.1:8080}"
