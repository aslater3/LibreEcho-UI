#!/bin/sh
# Service control in init/*.init.
#
# Regression cover for the failure where `stop` returned success without
# stopping anything because the pidfile was missing, so the following `start`
# added a second daemon instead of replacing the first.
#
# The helpers are evaluated straight out of a shipped init script rather than
# copied here, so this exercises the text that actually ships.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
SOURCE_INIT=init/libreecho-buttond.init
WORK=build/test-init-service-control
fails=0

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; fails=$((fails + 1)); }
check() { [ "$2" = "$3" ] && pass "$1 (=$3)" || fail "$1: expected $3, got $2"; }

# ---------------------------------------------------------------- structural
# Every script that manages a pidfile must have the process-based fallback,
# the stray sweep in stop, and the setsid detach in the start fallback.
echo "structural: all init scripts"
for f in init/*.init; do
    grep -q '^is_running()' "$f" || continue
    name=$(basename "$f")
    grep -q 'running_pids()' "$f" || fail "$name: no running_pids() fallback"
    grep -q 'for stray in \$(running_pids)' "$f" || fail "$name: stop does not sweep strays"
    if grep -q '\$DAEMON" \$ARGS </dev/null' "$f"; then
        grep -q 'setsid "\$DAEMON"' "$f" || fail "$name: start fallback does not setsid"
        start_body=$(sed -n '/^start_service()/,/^}/p' "$f")
        printf '%s\n' "$start_body" | grep -q 'sleep 1' && \
            fail "$name: start fallback still has a fixed one-second wait"
    fi
done
[ "$fails" -eq 0 ] && pass "every pidfile-managed init script has the guards"

# ---------------------------------------------------------------- behavioural
rm -rf "$WORK"; mkdir -p "$WORK"
DAEMON=$ROOT/$WORK/fake-daemon
PIDFILE=$ROOT/$WORK/fake.pid
LOGFILE=$ROOT/$WORK/fake.log
AUDIO_SOCK=$ROOT/$WORK/audio.sock
LED_SOCK=$ROOT/$WORK/led.sock
ARGS=
export DAEMON PIDFILE LOGFILE AUDIO_SOCK LED_SOCK ARGS

# A purpose-built ELF binary. A #!/bin/sh script reports its interpreter as
# argv[0], and coreutils is a multi-call binary that refuses to run under
# another name -- neither would exercise the argv[0] match the daemons rely on.
cat > "$WORK/fake.c" <<'EOC'
#include <unistd.h>
int main(void) { for (;;) pause(); return 0; }
EOC
CC=${CC:-$(command -v cc || command -v gcc || command -v clang || true)}
if [ -z "$CC" ]; then
    echo "  SKIP  behavioural checks (no C compiler)"
else
    "$CC" -w -o "$DAEMON" "$WORK/fake.c"

    # Pull the helpers, including the real fallback launcher, out of the
    # shipped script. buttond is used because its start helper has no hardware
    # prerequisites and can therefore be exercised as-is with temporary paths.
    HELPERS=$(sed -n '/^running_pids()/,/^}/p; /^pidfile_pid()/,/^}/p; /^is_running()/,/^}/p; /^terminate()/,/^}/p; /^start_service()/,/^}/p' "$SOURCE_INIT")
    eval "$HELPERS"

    count() { running_pids | wc -l | tr -d ' '; }

    echo "behavioural: pidfile-independent process control"
    "$DAEMON" & first=$!
    sleep 1
    check "running_pids finds the daemon" "$(count)" 1

    # The regression: no pidfile at all.
    rm -f "$PIDFILE"
    check "is_running without a pidfile" "$(is_running && echo yes || echo no)" yes

    # A stale pidfile naming a pid that is not ours must not be trusted.
    echo 999999 > "$PIDFILE"
    check "pidfile_pid rejects a dead pid" "$(pidfile_pid >/dev/null 2>&1 && echo yes || echo no)" no
    check "is_running still sees the process" "$(is_running && echo yes || echo no)" yes

    # A pidfile containing junk must not be trusted either.
    echo "not-a-pid" > "$PIDFILE"
    check "pidfile_pid rejects junk" "$(pidfile_pid >/dev/null 2>&1 && echo yes || echo no)" no

    # terminate() must actually reap it.
    for stray in $(running_pids); do terminate "$stray" 5; done
    check "terminate clears the process" "$(count)" 0
    kill -0 "$first" 2>/dev/null && kill -9 "$first" 2>/dev/null || true

    echo "behavioural: fallback start survives launcher exit"
    rm -f "$PIDFILE"
    # start-stop-daemon is normally in /usr/sbin; excluding sbin forces the
    # shipped setsid fallback while retaining setsid and the basic coreutils.
    (
        PATH=/usr/bin:/bin
        export PATH
        eval "$HELPERS"
        start_service
    )
    started=$(sed -n '1p' "$PIDFILE" 2>/dev/null || true)
    case "$started" in *[!0-9]*|'') started_live=no ;; *) kill -0 "$started" 2>/dev/null && started_live=yes || started_live=no ;; esac
    check "fallback writes a live pid" "$started_live" yes
    started_argv0=
    if [ "$started_live" = yes ] && [ -r "/proc/$started/cmdline" ]; then
        started_argv0=$(tr '\0' '\n' < "/proc/$started/cmdline" 2>/dev/null | sed -n '1p')
    fi
    check "pidfile identifies the daemon" "$started_argv0" "$DAEMON"
    # The launcher subshell has already exited at this point. The daemon must
    # still be present, proving the setsid path actually detached it.
    sleep 0.1
    check "daemon survives launcher shell exit" "$(kill -0 "$started" 2>/dev/null && echo yes || echo no)" yes
    for stray in $(running_pids); do terminate "$stray" 5; done
fi

rm -rf "$WORK"
echo
if [ "$fails" -eq 0 ]; then
    echo "test_init_service_control: OK"
else
    echo "test_init_service_control: $fails failure(s)"
    exit 1
fi
