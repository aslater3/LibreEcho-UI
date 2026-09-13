#!/bin/sh
# PR #245 finding 4: GET /api/v1/voice-pipeline must report
# home_assistant.ready only when the Wyoming satellite is actually usable --
# the wyomingd and waked daemons alive, the shared wake-word socket present, and
# something accepting on the Wyoming port -- not merely because a pidfile
# exists. This drives the shipped predicate itself (not a copy of it) through
# the readiness overrides, with both a positive and each negative transition.
#
#   1. every input live                       -> ready true
#   2. wake-word socket removed               -> ready false, restored -> true
#   3. waked daemon dead                      -> ready false
#   4. wyomingd daemon dead                   -> ready false
#   5. nothing listening on the Wyoming port  -> ready false, relistened -> true
#   6. synthetic socket table (override path) -> follows the table contents
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
PORT=${LIBREECHO_HA_READINESS_PORT:-18086}
PORT2=${LIBREECHO_HA_READINESS_PORT2:-18087}
WPORT=${LIBREECHO_HA_READINESS_WPORT:-27100}
WPORT2=${LIBREECHO_HA_READINESS_WPORT2:-27101}
WORK=build/test-ha-readiness
URL="http://127.0.0.1:$PORT"
URL2="http://127.0.0.1:$PORT2"
CFG="$WORK/config.json"
fails=0

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; fails=$((fails + 1)); }
check() { [ "$2" = "$3" ] && pass "$1 (=$3)" || fail "$1: expected $3, got $2"; }

command -v python3 >/dev/null 2>&1 || { echo "  SKIP  readiness checks (no python3)"; exit 0; }
[ -x ./build/libreecho-web ] || { echo "  FAIL  build/libreecho-web is missing"; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK"

server_pid=
server2_pid=
sock_pid=
sock2_pid=
tcp_pid=
tcp2_pid=
waked_pid=
wyomingd_pid=
replacement_pid=
waked2_pid=
wyomingd2_pid=

stop() { # $1=pid
    [ -n "$1" ] || return 0
    case "$1" in *[!0-9]*) return 0 ;; esac
    [ "$1" -gt 1 ] || return 0
    kill "$1" 2>/dev/null || true
}

cleanup() {
    for pid in "$server_pid" "$server2_pid" "$sock_pid" "$sock2_pid" "$tcp_pid" \
               "$tcp2_pid" "$waked_pid" "$wyomingd_pid" "$replacement_pid" \
               "$waked2_pid" "$wyomingd2_pid"; do
        stop "$pid"
    done
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# The shipped predicate reads the pidfiles, the wake-word socket and the
# listening table through overridable paths, so the test can point it at this
# build directory instead of /var/run.
export LIBREECHO_WYOMINGD_PIDFILE="$ROOT/$WORK/wyomingd.pid"
export LIBREECHO_WAKED_PIDFILE="$ROOT/$WORK/waked.pid"
export LIBREECHO_WAKEWORD_SOCK="$ROOT/$WORK/wakeword.sock"
export LIBREECHO_WYOMING_PORT="$WPORT"

# The listeners are helper scripts rather than inline here-docs: a backgrounded
# command carrying a here-doc runs in an intermediate subshell, so `$!` would
# name the subshell and cleanup would leak the listener.
cat > "$WORK/tcp-listen.py" <<'PY'
import socket, sys, time
server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", int(sys.argv[1])))
server.listen(1)
time.sleep(300)
PY
cat > "$WORK/unix-listen.py" <<'PY'
import os, socket, sys, time
try:
    os.unlink(sys.argv[1])
except OSError:
    pass
server = socket.socket(socket.AF_UNIX)
server.bind(sys.argv[1])
server.listen(1)
time.sleep(300)
PY

start_socket() { # $1=path -> starts the AF_UNIX listener and echoes its pid
    # Echoing the pid instead of backgrounding this function keeps `$!` on the
    # python process; a backgrounded function would report the wrapper subshell
    # and cleanup would leak the listener. The listener's own streams are
    # redirected so the command substitution does not wait on its open stdout.
    python3 "$WORK/unix-listen.py" "$1" >/dev/null 2>&1 &
    echo $!
}

start_tcp_listener() { # $1=port -> starts the TCP listener and echoes its pid
    python3 "$WORK/tcp-listen.py" "$1" >/dev/null 2>&1 &
    echo $!
}

wait_for_path() { # $1=path $2=test-flag
    i=0
    while [ ! "$2" "$1" ]; do
        i=$((i + 1))
        [ "$i" -lt 100 ] || return 1
        sleep 0.05
    done
    return 0
}

start_server() { # $1=port $2=config -> echoes pid
    ./build/libreecho-web --backend mock --config "$2" --mock-config ./config/mock-state.json \
        --web-root ./web --listen "127.0.0.1:$1" --seed 42 --dev-controls \
        >"$WORK/server-$1.log" 2>&1 &
    echo $!
}

wait_for_server() { # $1=url $2=pid
    i=0
    while ! curl -fsS "$1/api/v1/status" >/dev/null 2>&1; do
        i=$((i + 1))
        if [ "$i" -ge 60 ]; then
            cat "$WORK/server-$3.log" 2>/dev/null || true
            return 1
        fi
        kill -0 "$2" 2>/dev/null || return 1
        sleep 0.1
    done
}

ready_of() { # $1=url
    curl -fsS "$1/api/v1/voice-pipeline" | jq -c '.data.home_assistant'
}

csrf_of() { # $1=url
    curl -fsS "$1/api/v1/config" | jq -r '.data.csrf_token'
}

enable_ha() { # $1=url
    curl -fsS -X PUT "$1/api/v1/integrations/home-assistant" \
        -H "X-LibreEcho-CSRF: $(csrf_of "$1")" -H 'Content-Type: application/json' \
        --data '{"enabled":true}' >/dev/null
}

# ---------------------------------------------------------------- phase 1
# Real kernel table: a real listener on the ready port.
# A stale listener from an earlier run would make the negative transitions
# meaningless, so refuse to run against an already-listening port.
for check_port in "$WPORT" "$WPORT2"; do
    if awk -v port="$(printf '%04X' "$check_port")" \
        '$4 == "0A" && $2 ~ (":" port "$") { found = 1 } END { exit found ? 0 : 1 }' \
        /proc/net/tcp 2>/dev/null; then
        echo "  FAIL  port $check_port is already listening; a stale listener would invalidate the negatives"
        exit 1
    fi
done
sock_pid=$(start_socket "$LIBREECHO_WAKEWORD_SOCK")
tcp_pid=$(start_tcp_listener "$WPORT")
wait_for_path "$LIBREECHO_WAKEWORD_SOCK" -S || fail "wake-word socket did not appear"
sleep 300 & waked_pid=$!
sleep 300 & wyomingd_pid=$!
printf '%s\n' "$wyomingd_pid" > "$LIBREECHO_WYOMINGD_PIDFILE"
printf '%s\n' "$waked_pid" > "$LIBREECHO_WAKED_PIDFILE"

server_pid=$(start_server "$PORT" "$CFG")
wait_for_server "$URL" "$server_pid" "$PORT" || { fail "mock server did not become ready"; exit 1; }
enable_ha "$URL"
check "mode is home-assistant" \
    "$(curl -fsS "$URL/api/v1/voice-pipeline" | jq -r '.data.mode')" "home-assistant"

# 1. every readiness input live.
check "all inputs live reports ready" "$(ready_of "$URL")" '{"ready":true}'

# 2. the wake-word socket is required.
stop "$sock_pid"
rm -f "$LIBREECHO_WAKEWORD_SOCK"
check "missing wake-word socket reports not ready" "$(ready_of "$URL")" '{"ready":false}'
sock_pid=$(start_socket "$LIBREECHO_WAKEWORD_SOCK")
wait_for_path "$LIBREECHO_WAKEWORD_SOCK" -S || fail "wake-word socket did not reappear"
check "restored wake-word socket reports ready again" "$(ready_of "$URL")" '{"ready":true}'

# 2b. a stale regular file at the socket path is not a usable socket.
stop "$sock_pid"
rm -f "$LIBREECHO_WAKEWORD_SOCK"
printf 'stale\n' > "$LIBREECHO_WAKEWORD_SOCK"
check "regular file at the wake-word path reports not ready" "$(ready_of "$URL")" '{"ready":false}'
rm -f "$LIBREECHO_WAKEWORD_SOCK"
sock_pid=$(start_socket "$LIBREECHO_WAKEWORD_SOCK")
wait_for_path "$LIBREECHO_WAKEWORD_SOCK" -S || fail "wake-word socket did not reappear after the regular-file case"
check "socket restored after the regular-file case reports ready again" "$(ready_of "$URL")" '{"ready":true}'

# 3. the waked daemon behind the socket is required.
kill "$waked_pid" 2>/dev/null || true
wait "$waked_pid" 2>/dev/null || true
waked_pid=
check "dead waked daemon reports not ready" "$(ready_of "$URL")" '{"ready":false}'
sleep 300 & replacement_pid=$!
printf '%s\n' "$replacement_pid" > "$LIBREECHO_WAKED_PIDFILE"
check "restarted waked daemon reports ready again" "$(ready_of "$URL")" '{"ready":true}'

# 4. wyomingd itself is required.
kill "$wyomingd_pid" 2>/dev/null || true
wait "$wyomingd_pid" 2>/dev/null || true
wyomingd_pid=
check "dead wyomingd reports not ready" "$(ready_of "$URL")" '{"ready":false}'
sleep 300 & wyomingd_pid=$!
printf '%s\n' "$wyomingd_pid" > "$LIBREECHO_WYOMINGD_PIDFILE"
check "restarted wyomingd reports ready again" "$(ready_of "$URL")" '{"ready":true}'

# 5. a pidfile is not enough: the Wyoming port must be accepting.
port_listened() { # $1=port in hex; mirrors init/libreecho-web.init
    awk -v port="$1" '$4 == "0A" && $2 ~ (":" port "$") { found = 1 }
         END { exit found ? 0 : 1 }' /proc/net/tcp 2>/dev/null
}
wait_for_listen_state() { # $1=port in hex $2=yes|no
    i=0
    while :; do
        if [ "$2" = yes ]; then
            port_listened "$1" && return 0
        else
            port_listened "$1" || return 0
        fi
        i=$((i + 1))
        [ "$i" -lt 100 ] || return 1
        sleep 0.05
    done
}
WPORT_HEX=$(printf '%04X' "$WPORT")
kill "$tcp_pid" 2>/dev/null || true
wait "$tcp_pid" 2>/dev/null || true
tcp_pid=
wait_for_listen_state "$WPORT_HEX" no || fail "the closed port is still listed as listening"
check "closed Wyoming port reports not ready" "$(ready_of "$URL")" '{"ready":false}'
tcp_pid=$(start_tcp_listener "$WPORT")
wait_for_listen_state "$WPORT_HEX" yes || fail "the Wyoming port did not come back"
check "relistened Wyoming port reports ready again" "$(ready_of "$URL")" '{"ready":true}'

# ---------------------------------------------------------------- phase 2
# The listening table path is overridable, so readiness follows its contents.
TAB="$ROOT/$WORK/net-tcp"
write_table() { # $1=port in hex, or empty for "not listening"
    {
        printf '  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n'
        if [ -n "$1" ]; then
            printf '   0: 00000000:%s 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 1 1 0000000000000000 100 0 0 10 0\n' "$1"
        fi
        printf '   1: 0100007F:1F90 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 2 1 0000000000000000 100 0 0 10 0\n'
    } > "$TAB"
}
PORT2_HEX=$(printf '%04X' "$WPORT2")
write_table "$PORT2_HEX"
sleep 300 & waked2_pid=$!
sleep 300 & wyomingd2_pid=$!
printf '%s\n' "$wyomingd2_pid" > "$WORK/wyomingd2.pid"
printf '%s\n' "$waked2_pid" > "$WORK/waked2.pid"
sock2_pid=$(start_socket "$WORK/wakeword2.sock")
wait_for_path "$WORK/wakeword2.sock" -S || fail "second wake-word socket did not appear"

LIBREECHO_WYOMINGD_PIDFILE="$ROOT/$WORK/wyomingd2.pid" \
LIBREECHO_WAKED_PIDFILE="$ROOT/$WORK/waked2.pid" \
LIBREECHO_WAKEWORD_SOCK="$ROOT/$WORK/wakeword2.sock" \
LIBREECHO_WYOMING_PORT="$WPORT2" \
LIBREECHO_PROC_NET_TCP="$TAB" \
    ./build/libreecho-web --backend mock --config "$CFG" \
    --mock-config ./config/mock-state.json --web-root ./web \
    --listen "127.0.0.1:$PORT2" --seed 42 --dev-controls \
    >"$WORK/server-$PORT2.log" 2>&1 &
server2_pid=$!
i=0
while ! curl -fsS "$URL2/api/v1/status" >/dev/null 2>&1; do
    i=$((i + 1))
    [ "$i" -lt 60 ] || { cat "$WORK/server-$PORT2.log" 2>/dev/null || true; fail "second server did not become ready"; break; }
    kill -0 "$server2_pid" 2>/dev/null || { fail "second server exited"; break; }
    sleep 0.1
done
if curl -fsS "$URL2/api/v1/status" >/dev/null 2>&1; then
    enable_ha "$URL2"
    check "synthetic table naming the port reports ready" "$(ready_of "$URL2")" '{"ready":true}'
    write_table ""
    check "synthetic table without the port reports not ready" "$(ready_of "$URL2")" '{"ready":false}'
    write_table "$PORT2_HEX"
    check "rewritten synthetic table reports ready again" "$(ready_of "$URL2")" '{"ready":true}'
fi

rm -rf "$WORK"
echo
if [ "$fails" -eq 0 ]; then
    echo "test_voice_pipeline_ha_readiness: OK"
else
    echo "test_voice_pipeline_ha_readiness: $fails failure(s)"
    exit 1
fi
