#!/bin/sh
set -eu
PORT=${LIBREECHO_IDENTITY_TEST_PORT:-18087}
ROOT=./build/test-identity
CFG=./build/test-identity-config.json
rm -rf "$ROOT"
mkdir -p "$ROOT"
printf '%s\n' 'console=tty0 androidboot.serialno=TESTDEVICE1234 root=/dev/ram' >"$ROOT/cmdline"
printf '{"integrations":0}\n' >"$CFG"
LIBREECHO_CMDLINE_PATH="$ROOT/cmdline" \
./build/libreecho-web --backend linux --config "$CFG" --web-root ./web \
    --listen "127.0.0.1:$PORT" >./build/test-identity.log 2>&1 &
pid=$!
cleanup(){ kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; }
trap cleanup EXIT INT TERM
for i in $(seq 1 30); do
    device=$(curl -fsS "http://127.0.0.1:$PORT/api/v1/device" 2>/dev/null) && break
    sleep 0.1
done
printf '%s' "$device" | jq -e '.ok and (.data.serial | test("^device-[0-9a-f]{16}$"))' >/dev/null
! printf '%s' "$device" | grep -q 'TESTDEVICE1234'
kill "$pid"; wait "$pid" 2>/dev/null || true
pid=0
printf '%s\n' 'console=tty0 root=/dev/ram' >"$ROOT/cmdline"
LIBREECHO_CMDLINE_PATH="$ROOT/cmdline" \
./build/libreecho-web --backend linux --config "$CFG" --web-root ./web \
    --listen "127.0.0.1:$PORT" >./build/test-identity-missing.log 2>&1 &
pid=$!
for i in $(seq 1 30); do
    device=$(curl -fsS "http://127.0.0.1:$PORT/api/v1/device" 2>/dev/null) && break
    sleep 0.1
done
printf '%s' "$device" | jq -e '.ok and .data.serial == "unavailable"' >/dev/null
printf '%s\n' 'device identity fallback: ok'
