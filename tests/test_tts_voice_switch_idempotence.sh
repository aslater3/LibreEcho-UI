#!/bin/sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/libreecho-ttsd-switch.XXXXXX")
SERVICE_PID=0
SOCKET_PID=0
cleanup() {
    if [ "$SERVICE_PID" -gt 1 ]; then
        kill "$SERVICE_PID" 2>/dev/null || true
        wait "$SERVICE_PID" 2>/dev/null || true
    fi
    if [ "$SOCKET_PID" -gt 1 ]; then
        kill "$SOCKET_PID" 2>/dev/null || true
        wait "$SOCKET_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

VOICE_FILE=$WORK/tts-voice
PIDFILE=$WORK/ttsd.pid
SOCKET=$WORK/tts.sock
LOGFILE=$WORK/ttsd.log
PAYLOAD=$WORK/missing-payload.squashfs
RUNTIME_ROOT=$WORK/runtime
CONFIG=$WORK/config.json

printf '%s\n' northern-male >"$VOICE_FILE"
: >"$LOGFILE"

sleep 30 &
SERVICE_PID=$!
printf '%s\n' "$SERVICE_PID" >"$PIDFILE"

python3 - "$SOCKET" <<'PY' &
import socket
import sys
import time

server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(sys.argv[1])
server.listen(1)
time.sleep(30)
PY
SOCKET_PID=$!

attempt=0
while [ ! -S "$SOCKET" ]; do
    attempt=$((attempt + 1))
    [ "$attempt" -lt 50 ] || {
        echo 'FAIL: test socket did not appear' >&2
        exit 1
    }
    sleep 0.02
done

before_hash=$(sha256sum "$VOICE_FILE" | awk '{print $1}')

if ! VOICE_FILE="$VOICE_FILE" PIDFILE="$PIDFILE" SOCKET="$SOCKET" \
    LOGFILE="$LOGFILE" PAYLOAD="$PAYLOAD" RUNTIME_ROOT="$RUNTIME_ROOT" \
    CONFIG="$CONFIG" sh "$ROOT/init/libreecho-ttsd.init" switch northern-male \
    >"$WORK/switch.log" 2>&1; then
    echo 'FAIL: same-value voice switch was not accepted' >&2
    sed -n '1,120p' "$WORK/switch.log" >&2
    exit 1
fi

after_hash=$(sha256sum "$VOICE_FILE" | awk '{print $1}')
[ "$before_hash" = "$after_hash" ]

IFS= read -r pid_after <"$PIDFILE"
[ "$pid_after" = "$SERVICE_PID" ]
kill -0 "$SERVICE_PID" 2>/dev/null
[ -S "$SOCKET" ]

printf '%s\n' 'tts same-value voice switch idempotence: ok'
