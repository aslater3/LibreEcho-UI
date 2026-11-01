#!/bin/sh
# Overlapping start/stop/restart calls of the agentd init script must leave
# exactly one daemon (issue aslater3/LibreEcho#344). Runs the real script with
# a stand-in daemon and a slowed-down payload mount so both callers are inside
# start_service at the same time.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SOURCE=${AGENTD_INIT:-$ROOT/init/libreecho-agentd.init}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/libreecho-agentd-lock.XXXXXX")
cleanup() {
    for pid in $(daemon_pids 2>/dev/null); do kill -KILL "$pid" 2>/dev/null || true; done
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

command -v flock >/dev/null 2>&1 || { echo 'agentd init lifecycle lock: skipped (no flock)'; exit 0; }
command -v setsid >/dev/null 2>&1 || { echo 'agentd init lifecycle lock: skipped (no setsid)'; exit 0; }

# Absolute path of a tool on PATH. BusyBox sh answers "command -v sh" with the
# bare applet name, which is no use for a symlink or a #! line.
tool() {
    old_ifs=$IFS; IFS=:
    for dir in $PATH; do
        if [ -x "$dir/$1" ] && [ ! -d "$dir/$1" ]; then IFS=$old_ifs; printf '%s\n' "$dir/$1"; return 0; fi
    done
    IFS=$old_ifs
    echo "missing tool: $1" >&2
    return 1
}
SH=$(tool sh)
MKDIR=$(tool mkdir)

RUNTIME_ROOT=$WORK/root
DAEMON=$RUNTIME_ROOT/usr/local/sbin/libreecho-agentd
mkdir -p "$WORK/bin" "$(dirname "$DAEMON")" "$RUNTIME_ROOT/usr/local/libexec" \
    "$RUNTIME_ROOT/usr/local/share/libreecho"
# A real executable at $DAEMON keeps argv[0] equal to the path, which is what
# running_pids matches on. (A copy of sleep does not work where coreutils is a
# multi-call binary that dispatches on its name.)
printf '#include <unistd.h>\nint main(void){sleep(60);return 0;}\n' >"$WORK/stub.c"
${CC:-cc} -o "$DAEMON" "$WORK/stub.c"
cp "$DAEMON" "$RUNTIME_ROOT/usr/local/libexec/libreecho-curl"
: >"$RUNTIME_ROOT/usr/local/share/libreecho/cacert.pem"
: >"$WORK/payload.squashfs"
printf '%s\n' redistributable >"$WORK/feature-policy"
python3 - "$WORK" <<'PY'
import socket, sys
for name in ('stt.sock', 'audio.sock', 'tts.sock'):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(sys.argv[1] + '/' + name)
    s.close()
PY

# Only the tools the script needs, so the setsid launch path is used on hosts
# that also have a Debian start-stop-daemon. mount is slowed down to widen the
# window between the "not running" check and the launch.
for tool in sh tr sed sleep kill grep rm sort dirname cat flock setsid date; do
    ln -s "$(tool "$tool")" "$WORK/bin/$tool"
done
printf '#!%s\nsleep 1\n' "$SH" >"$WORK/bin/mount"
printf '#!%s\nexit 0\n' "$SH" >"$WORK/bin/umount"
printf '#!%s\nexit 0\n' "$SH" >"$WORK/bin/chmod"
# start_service creates /data/libreecho/*; keep the host's /data untouched.
cat >"$WORK/bin/mkdir" <<EOF
#!$SH
for a in "\$@"; do shift; case "\$a" in /data/*) ;; *) set -- "\$@" "\$a" ;; esac; done
exec $MKDIR "\$@"
EOF
chmod +x "$WORK/bin/mount" "$WORK/bin/umount" "$WORK/bin/chmod" "$WORK/bin/mkdir"

daemon_pids() {
    for entry in /proc/[0-9]*; do
        argv0=$({ tr '\0' '\n' <"$entry/cmdline"; } 2>/dev/null | sed -n '1p')
        [ "$argv0" = "$DAEMON" ] && printf '%s\n' "${entry#/proc/}"
    done
    return 0
}

run_init() {
    env -i PATH="$WORK/bin" \
        PAYLOAD="$WORK/payload.squashfs" RUNTIME_ROOT="$RUNTIME_ROOT" \
        PIDFILE="$WORK/agentd.pid" LOGFILE="$WORK/agentd.log" \
        SOCKET="$WORK/agent.sock" STT_SOCKET="$WORK/stt.sock" \
        AUDIO_SOCKET="$WORK/audio.sock" TTS_SOCKET="$WORK/tts.sock" \
        WAKE_SOCKET="$WORK/wakeword.sock" FEATURE_POLICY_FILE="$WORK/feature-policy" \
        LOCKFILE="$WORK/agentd.lock" ARGS=60 \
        AGENT_LOCK_TIMEOUT_SECONDS="${LOCK_TIMEOUT:-30}" \
        sh "$SOURCE" "$@"
}

expect_count() {
    count=$(daemon_pids | wc -l | tr -d ' ')
    [ "$count" -eq "$1" ] || {
        echo "FAIL: $2: expected $1 agentd, found $count" >&2
        exit 1
    }
}

# The reproduction from the issue: two restarts at once.
run_init restart >/dev/null 2>&1 &
first=$!
run_init restart >/dev/null 2>&1 &
second=$!
wait "$first"
wait "$second"
expect_count 1 'concurrent restart'
[ "$(cat "$WORK/agentd.pid")" = "$(daemon_pids)" ] || {
    echo 'FAIL: pidfile does not name the running agentd' >&2
    exit 1
}

# Restart racing a plain start: start must see the daemon, not add one.
run_init restart >/dev/null 2>&1 &
first=$!
run_init start >/dev/null 2>&1 &
second=$!
wait "$first"
wait "$second"
expect_count 1 'restart racing start'

# The daemon must not inherit the lock, or the next caller would block on it.
flock -n "$WORK/agentd.lock" true || {
    echo 'FAIL: running agentd holds the lifecycle lock' >&2
    exit 1
}

run_init stop >/dev/null 2>&1
expect_count 0 'stop'

# A caller that cannot get the lock in time fails and starts nothing.
flock "$WORK/agentd.lock" sleep 3 &
holder=$!
sleep 0.3
if LOCK_TIMEOUT=1 run_init start >/dev/null 2>"$WORK/timeout.err"; then
    echo 'FAIL: start succeeded while the lock was held' >&2
    exit 1
fi
grep -q 'did not finish within 1s' "$WORK/timeout.err"
expect_count 0 'lock timeout'
wait "$holder"

echo 'agentd init lifecycle lock: ok'
