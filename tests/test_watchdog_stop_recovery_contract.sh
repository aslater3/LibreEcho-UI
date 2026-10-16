#!/bin/sh
# Stopping the supervisor must also stop the recovery it launched.
#
# libreecho-watchdogd restarts a service that stopped answering by forking that
# service's init script, and such a start can run a long time before it launches
# the daemon or touches persistent state: libreecho-agentd waits up to 90
# seconds for its dependency sockets. A caller that stops the supervisor to
# quiesce a service -- the factory reset stops it first for exactly that reason
# -- must be able to rely on that stop covering a recovery that is already in
# flight, or the service starts after the caller confirmed the stop and
# re-creates the state the caller is removing.
#
# The helpers are evaluated straight out of the shipped init script, and the
# supervisor and its recovery are real processes, so this runs the text that
# ships.
#
# Liveness is read from the process state rather than from kill -0 alone. On a
# host where PID 1 does not promptly reap orphans -- a container without a
# reaping init -- the recovery killed after its supervisor exited stays visible
# in /proc with state Z, and kill -0 still succeeds for it, so both of these
# checks reported a stopped recovery as alive. A process counts as alive only
# while it really is one, which keeps a genuinely running recovery visible.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
SOURCE_INIT=init/libreecho-watchdogd.init
WORK=build/test-watchdog-stop-recovery
fails=0

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; fails=$((fails + 1)); }
check() { [ "$2" = "$3" ] && pass "$1 (=$3)" || fail "$1: expected $3, got $2"; }

# The state letter of a live process, or nothing once the process is gone. The
# comm field can contain spaces and parentheses, so the state is read after the
# final ')' instead of by field number.
process_state() {
    [ -r "/proc/$1/stat" ] || return 0
    sed -n 's/^[0-9][0-9]* (.*) \([A-Z]\) .*$/\1/p' "/proc/$1/stat" | sed -n '1p'
}

# Alive means the process still exists and is not a zombie or already reaped.
# Anything else -- including a recovery that is still running -- is alive.
alive() {
    case "$(process_state "$1")" in
        ''|Z|X) echo no ;;
        *) echo yes ;;
    esac
}

# ---------------------------------------------------------------- structural
echo "structural: $SOURCE_INIT"
grep -q '^children_of()' "$SOURCE_INIT" || fail "no children_of() helper"
stop_body=$(sed -n '/^stop_service()/,/^}/p' "$SOURCE_INIT")
printf '%s\n' "$stop_body" | grep -q 'children_of' || \
    fail "stop does not collect the recovery"
# The children must be read while the daemon is still alive: a dead daemon's
# children are reparented and cannot be found by parent any more.
collected=$(printf '%s\n' "$stop_body" | grep -n 'children_of' | sed -n '1p' | cut -d: -f1)
terminated=$(printf '%s\n' "$stop_body" | grep -n 'start-stop-daemon -K' | sed -n '1p' | cut -d: -f1)
if [ -n "$collected" ] && [ -n "$terminated" ] && [ "$collected" -lt "$terminated" ]; then
    pass "recovery is collected before the supervisor is terminated"
else
    fail "recovery is collected only after the supervisor is terminated"
fi

# ---------------------------------------------------------------- behavioural
rm -rf "$WORK"; mkdir -p "$WORK"
DAEMON=$ROOT/$WORK/fake-watchdog
PIDFILE=$ROOT/$WORK/fake.pid
LOGFILE=$ROOT/$WORK/fake.log
RECOVERY_SCRIPT=$ROOT/$WORK/recovery.sh
export DAEMON PIDFILE LOGFILE RECOVERY_SCRIPT

CC=${CC:-$(command -v cc || command -v gcc || command -v clang || true)}
if [ -z "$CC" ]; then
    echo "  SKIP  behavioural checks (no C compiler)"
else
    # A purpose-built ELF: the init script matches the daemon on argv[0], which
    # a shell script could not present. The child it forks mirrors a recovery
    # exactly -- /bin/sh <init script> start -- and the script only waits, the
    # way a start waits for its dependencies before it launches anything.
    cat > "$WORK/fake-watchdog.c" <<'EOC'
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(void)
{
    const char *script = getenv("RECOVERY_SCRIPT");
    const char *zombie_file = getenv("ZOMBIE_PIDFILE");

    if (script && script[0]) {
        pid_t child = fork();

        if (child == 0) {
            execl("/bin/sh", "/bin/sh", script, "start", (char *)NULL);
            _exit(127);
        }
    }
    /* ZOMBIE_PIDFILE asks for a child that exits and is deliberately never
       reaped, so the fixture can show that a zombie is classified as stopped
       even though kill -0 still succeeds for it. */
    if (zombie_file && zombie_file[0]) {
        pid_t dead = fork();

        if (dead == 0)
            _exit(0);
        {
            FILE *file = fopen(zombie_file, "w");

            if (file) {
                fprintf(file, "%d\n", (int)dead);
                fclose(file);
            }
        }
    }
    for (;;)
        pause();
    return 0;
}
EOC
    "$CC" -w -o "$DAEMON" "$WORK/fake-watchdog.c"

    # ------------------------------------------------------------ classification
    # The stop checks below read liveness from the process state, so this pins
    # down what that reading means: a child that has exited without being reaped
    # is not alive, while a process that is still running is. PID 1 reaps
    # promptly on most hosts, so the zombie is created on purpose here rather
    # than left to the host's reaping policy.
    echo "classification: a zombie is stopped, a live process is not"
    ZOMBIE_PIDFILE=$ROOT/$WORK/zombie.pid
    rm -f "$ZOMBIE_PIDFILE"
    (unset RECOVERY_SCRIPT; ZOMBIE_PIDFILE=$ZOMBIE_PIDFILE; export ZOMBIE_PIDFILE; exec "$DAEMON") &
    zombie_holder=$!
    tries=0
    while [ ! -s "$ZOMBIE_PIDFILE" ] && [ "$tries" -lt 100 ]; do
        tries=$((tries + 1))
        sleep 0.05
    done
    zombie_pid=$(sed -n '1p' "$ZOMBIE_PIDFILE" 2>/dev/null || true)
    if [ -z "$zombie_pid" ]; then
        fail "the fixture could not create a zombie to classify"
    else
        check "the un-reaped child really is a zombie" "$(process_state "$zombie_pid")" Z
        check "zombie classified as stopped" "$(alive "$zombie_pid")" no
        check "live process classified as running" "$(alive "$zombie_holder")" yes
        # kill -0 alone is the reading this replaced, and it is what made a
        # stopped recovery look alive on a host that had not reaped it yet.
        if kill -0 "$zombie_pid" 2>/dev/null; then kill0=yes; else kill0=no; fi
        check "kill -0 alone would still call the zombie alive" "$kill0" yes
    fi
    kill -9 "$zombie_holder" 2>/dev/null || true
    wait "$zombie_holder" 2>/dev/null || true
    cat > "$RECOVERY_SCRIPT" <<'EOC'
#!/bin/sh
sleep 5
EOC
    chmod +x "$RECOVERY_SCRIPT"

    HELPERS=$(sed -n '/^running_pids()/,/^}/p; /^pidfile_pid()/,/^}/p; /^children_of()/,/^}/p; /^is_running()/,/^}/p; /^terminate()/,/^}/p; /^stop_service()/,/^}/p' "$SOURCE_INIT")
    eval "$HELPERS"

    if ! type children_of >/dev/null 2>&1; then
        echo "  SKIP  behavioural checks (the shipped script has no children_of())"
    else
        supervisor_pid=
        recovery_pid=
        # The supervisor is a direct child of this shell, so it can be reaped
        # here. The recovery cannot: it is reparented when the supervisor dies,
        # which is why its liveness is classified instead of waited on.
        reap_supervisor() {
            [ -n "$supervisor_pid" ] || return 0
            wait "$supervisor_pid" 2>/dev/null || true
        }
        cleanup() {
            [ -n "$recovery_pid" ] && kill -9 "$recovery_pid" 2>/dev/null || true
            if [ -n "$supervisor_pid" ]; then
                kill -9 "$supervisor_pid" 2>/dev/null || true
                reap_supervisor
            fi
        }
        trap cleanup EXIT INT TERM

        start_supervisor() {
            rm -f "$PIDFILE"
            "$DAEMON" &
            supervisor_pid=$!
            recovery_pid=
            tries=0
            while [ -z "$recovery_pid" ]; do
                tries=$((tries + 1))
                [ "$tries" -lt 100 ] || return 1
                recovery_pid=$(children_of "$supervisor_pid" | sed -n '1p')
                [ -n "$recovery_pid" ] || sleep 0.05
            done
            return 0
        }

        # start-stop-daemon is normally in /usr/sbin, which is the branch the
        # image takes when the pidfile is present.
        if command -v start-stop-daemon >/dev/null 2>&1; then
            echo "behavioural: stop with a live pidfile"
            start_supervisor || fail "the supervisor never forked a recovery"
            echo "$supervisor_pid" > "$PIDFILE"
            # The positive control for the classification: a recovery that is
            # still running must be visible as alive, or the state reading
            # would pass these checks by calling everything stopped.
            check "recovery in flight is alive before the stop" "$(alive "$recovery_pid")" yes
            stop_service
            reap_supervisor
            check "supervisor stopped" "$(alive "$supervisor_pid")" no
            check "recovery in flight stopped" "$(alive "$recovery_pid")" no
            check "pidfile removed" "$([ -e "$PIDFILE" ] && echo yes || echo no)" no
            cleanup
        else
            echo "  SKIP  stop with a live pidfile (no start-stop-daemon)"
        fi

        # No pidfile: the stop falls back to the process sweep, and the recovery
        # has to be covered there too.
        echo "behavioural: stop without a pidfile"
        start_supervisor || fail "the supervisor never forked a recovery"
        recovery_before=$recovery_pid
        check "recovery is alive before the stop without a pidfile" "$(alive "$recovery_before")" yes
        stop_service
        reap_supervisor
        check "supervisor stopped without a pidfile" "$(alive "$supervisor_pid")" no
        check "recovery stopped without a pidfile" "$(alive "$recovery_before")" no
        cleanup
    fi
fi

trap - EXIT INT TERM
rm -rf "$WORK"
echo
if [ "$fails" -eq 0 ]; then
    echo "test_watchdog_stop_recovery_contract: OK"
else
    echo "test_watchdog_stop_recovery_contract: $fails failure(s)"
    exit 1
fi
