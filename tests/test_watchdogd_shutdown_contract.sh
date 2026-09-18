#!/bin/sh
# Stopping the supervisor must not leave a recovery behind it, including one it
# launches between the descendants being enumerated and its own exit.
#
# A factory reset stops the supervisor first and then the daemons it watches.
# The init script collects the supervisor's descendants while the daemon is
# still alive (tests/test_watchdog_stop_recovery_contract.sh), which covers a
# recovery that is already running. It cannot cover a recovery forked after
# that one-time snapshot, and a SIGTERM on its own does not prevent one: the
# handler only clears the daemon's run flag, and the recovery path used to wait
# a child out across the signal. A recovery forked in that window is unrecorded,
# is reparented when the supervisor exits, and finishes starting a service whose
# stop the caller has already confirmed -- and a slow service start (agentd's
# init waits up to 90 seconds for its dependencies) is exactly what makes the
# window wide enough to matter.
#
# This fixture drives the real daemon across both halves of that transition.
# The point it has to be deterministic at is "the signal arrives while the
# recovery path is live", so the recovery announces itself from inside the fork
# (it is the process the snapshot would have to have seen) and the fixture
# signals only after that. Every invocation records whether the stop request
# had already been issued, so a recovery launched after the signal is visible as
# such rather than inferred from timing.
#
#   case 1  the signal lands while a recovery is running
#           -> the supervisor leaves promptly, the recovery and the work it
#              started are gone, and the start it was working towards never
#              runs
#   case 2  the signal lands inside the stop sweep of a group whose start sweep
#           has not run yet
#           -> no further member is stopped and none is started
#
# Liveness is read from the process state, as in the stop-recovery fixture: a
# process that has exited without being reaped still answers kill -0, and
# calling that alive would make a stopped recovery look like a survivor.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
WORK=build/test-watchdogd-shutdown
WD=${WATCHDOGD:-./build/libreecho-watchdogd}
fails=0

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; fails=$((fails + 1)); }
check() { [ "$2" = "$3" ] && pass "$1 (=$3)" || fail "$1: expected $3, got $2"; }

if [ ! -x "$WD" ]; then
    echo "test_watchdogd_shutdown_contract: $WD is not built"
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "test_watchdogd_shutdown_contract: SKIP (no python3 to stand in for a service)"
    exit 0
fi

rm -rf "$WORK"
mkdir -p "$WORK"
: > "$WORK/invocations"
SENTINEL=$WORK/term-sent

cleanup() {
    [ -n "${wd:-}" ] && kill -9 "$wd" 2>/dev/null || true
    for pidfile in "$WORK"/*.pid; do
        [ -f "$pidfile" ] && kill -9 "$(cat "$pidfile")" 2>/dev/null || true
    done
    return 0
}
trap cleanup EXIT INT TERM

# The state letter of a live process, or nothing once the process is gone. The
# comm field can contain spaces and parentheses, so the state is read after the
# final ')' instead of by field number.
process_state() {
    [ -r "/proc/$1/stat" ] || return 0
    sed -n 's/^[0-9][0-9]* (.*) \([A-Z]\) .*$/\1/p' "/proc/$1/stat" 2>/dev/null | sed -n '1p' || true
}

alive() {
    case "$(process_state "$1")" in
        ''|Z|X) echo no ;;
        *) echo yes ;;
    esac
}

# A process whose parent, or whose process group, is the one asked for.
pids_with() {
    field=$1
    wanted=$2
    for stat in /proc/[0-9]*/stat; do
        [ -r "$stat" ] || continue
        pid=${stat#/proc/}
        pid=${pid%/stat}
        have=$(sed -n "s/^[0-9][0-9]* (.*) [A-Z] \\([0-9][0-9]*\\) \\([0-9][0-9]*\\) .*$/\\$field/p" "$stat" 2>/dev/null | sed -n '1p' || true)
        [ "$have" = "$wanted" ] && echo "$pid"
    done
    return 0
}

children_of() { pids_with 1 "$1"; }
group_of() { pids_with 2 "$1"; }

live_group_members() {
    for pid in $(group_of "$1"); do
        [ "$(alive "$pid")" = yes ] && echo "$pid"
    done
    return 0
}

# A minimal service: answers the adapter "status" call and nothing else.
cat > "$WORK/fake.py" <<'PY'
import os, socket, sys
path = sys.argv[1]
try: os.unlink(path)
except FileNotFoundError: pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(path); s.listen(4)
open(sys.argv[2], "w").write(str(os.getpid()))
while True:
    c, _ = s.accept()
    try:
        c.recv(4096)
        c.sendall(b'{"v":1,"id":1,"ok":true,"data":{},"error":null}\n')
    except Exception:
        pass
    c.close()
PY

# An "init script" for a supervised service. It records every invocation with
# whether the stop request had already been issued -- the one thing that
# distinguishes "the supervisor launched this before it was stopped" from "the
# supervisor launched this after" -- and starts a real service on `start`.
# $4 is the work its `stop` action does, which is where a slow service start
# spends its time.
write_init() {
    cat > "$1" <<EOF
#!/bin/sh
flag=before
[ -e "$SENTINEL" ] && flag=after
echo "\$flag $2 \$1" >> "$WORK/invocations"
case "\$1" in
  start)
    touch "$WORK/$2.started"
    python3 "$WORK/fake.py" "$3" "$WORK/$2.pid" &
    ;;
  stop)
    [ -f "$WORK/$2.pid" ] && kill "\$(cat "$WORK/$2.pid")" 2>/dev/null || true
    rm -f "$3"
$4
    ;;
esac
exit 0
EOF
    chmod +x "$1"
}

# Wait for a file to appear, in 50 ms steps.
wait_for() {
    tries=0
    while [ ! -e "$1" ]; do
        tries=$((tries + 1))
        [ "$tries" -lt 200 ] || return 1
        sleep 0.05
    done
    return 0
}

# Kill the supervised service so the supervisor sees it fail twice, then wait
# for the recovery to be inside the fork under test.
break_service() {
    [ -f "$WORK/$1.pid" ] && kill "$(cat "$WORK/$1.pid")" 2>/dev/null || true
    rm -f "$WORK/$1.sock"
    return 0
}

# The process group of a recovery: cancelled recoveries are killed as a group,
# so this is what has to be empty afterwards.
recovery_group_of() {
    sed -n 's/^[0-9][0-9]* (.*) [A-Z] [0-9][0-9]* \([0-9][0-9]*\) .*$/\1/p' \
        "/proc/$1/stat" 2>/dev/null | sed -n '1p' || true
}

# ---------------------------------------------------------------- case 1
echo "case 1: the stop request lands while a recovery is running"
SOCK_1=$WORK/writer.sock
INIT=$WORK/writer.init
write_init "$INIT" writer "$SOCK_1" "    touch \"$WORK/writer.stop-began\"
    sleep 5
    touch \"$WORK/writer.stop-finished\""

# The service is up and answering, so the supervisor latches on it as healthy,
# and the log starts clean for the run under test.
sh "$INIT" start
sleep 1
: > "$WORK/invocations"
rm -f "$WORK/writer.started"
rm -f "$SENTINEL"

( sleep 3; break_service writer ) &
"$WD" --passes 12 --interval 1 --service "writer:$SOCK_1:$INIT" >"$WORK/writer.log" 2>&1 &
wd=$!

if ! wait_for "$WORK/writer.stop-began"; then
    fail "the supervisor never started a recovery to interrupt"
else
    recovery=$(children_of "$wd" | sed -n '1p')
    check "the recovery in flight is alive before the signal" "$(alive "$recovery")" yes
    recovery_group=$(recovery_group_of "$recovery")

    # The request is visible to any invocation that begins now, which is what
    # makes a recovery launched after this point identifiable.
    : > "$SENTINEL"
    signaled=$(date +%s)
    kill -TERM "$wd"

    waited=0
    while [ "$(alive "$wd")" = yes ] && [ "$waited" -lt 200 ]; do
        waited=$((waited + 1))
        sleep 0.05
    done
    elapsed=$(( $(date +%s) - signaled ))
    check "the supervisor left promptly instead of waiting the recovery out" \
        "$([ "$elapsed" -lt 4 ] && echo yes || echo no)" yes
    check "the supervisor stopped" "$(alive "$wd")" no
    wait "$wd" 2>/dev/null || true
    wd=

    check "no recovery was launched after the stop request" \
        "$(grep -c '^after ' "$WORK/invocations" || true)" 0
    check "the cancelled recovery did not finish its work" \
        "$([ -e "$WORK/writer.stop-finished" ] && echo yes || echo no)" no
    check "nothing the recovery started is still running" \
        "$([ -n "$recovery_group" ] && live_group_members "$recovery_group" | wc -l | tr -d ' ' || echo unknown)" 0
    check "the service was not started after the stop request" \
        "$([ -e "$WORK/writer.started" ] && echo yes || echo no)" no
    check "the socket did not come back" "$([ -S "$SOCK_1" ] && echo yes || echo no)" no
fi

# ---------------------------------------------------------------- case 2
# A group stops in reverse table order and starts in table order, so the signal
# is aimed at the gap between the two sweeps: the consumer's stop is in flight,
# the producer has not been stopped, and no member has been started. A stop
# request that is not honoured here leaves the producer to be stopped and then
# both members restarted -- a fork after the snapshot, on a service the caller
# has already quiesced.
echo "case 2: the stop request lands inside the stop sweep of a group"
SOCK_P=$WORK/producer.sock
SOCK_C=$WORK/consumer.sock
INIT_P=$WORK/producer.init
INIT_C=$WORK/consumer.init
write_init "$INIT_P" producer "$SOCK_P" ""
write_init "$INIT_C" consumer "$SOCK_C" "    touch \"$WORK/consumer.stop-began\"
    sleep 5
    touch \"$WORK/consumer.stop-finished\""

sh "$INIT_P" start
sh "$INIT_C" start
sleep 1
: > "$WORK/invocations"
rm -f "$WORK/producer.started" "$WORK/consumer.started"
rm -f "$SENTINEL"

# The consumer is the group's second member, so it is the one the stop sweep
# reaches first; killing it alone makes the group unhealthy, as in the
# recovery fixture.
( sleep 3; break_service consumer ) &
"$WD" --passes 12 --interval 1 \
    --service "producer:$SOCK_P:$INIT_P:capture" \
    --service "consumer:$SOCK_C:$INIT_C:capture" >"$WORK/group.log" 2>&1 &
wd=$!

if ! wait_for "$WORK/consumer.stop-began"; then
    fail "the supervisor never entered the group's stop sweep"
else
    recovery=$(children_of "$wd" | sed -n '1p')
    check "the group recovery in flight is alive before the signal" "$(alive "$recovery")" yes
    recovery_group=$(recovery_group_of "$recovery")

    : > "$SENTINEL"
    kill -TERM "$wd"

    waited=0
    while [ "$(alive "$wd")" = yes ] && [ "$waited" -lt 200 ]; do
        waited=$((waited + 1))
        sleep 0.05
    done
    check "the supervisor stopped" "$(alive "$wd")" no
    wait "$wd" 2>/dev/null || true
    wd=

    check "no recovery was launched after the stop request" \
        "$(grep -c '^after ' "$WORK/invocations" || true)" 0
    check "the producer was not stopped after the stop request" \
        "$(grep -c '^after producer stop' "$WORK/invocations" || true)" 0
    check "no member of the group was started" \
        "$([ -e "$WORK/producer.started" ] || [ -e "$WORK/consumer.started" ] && echo yes || echo no)" no
    check "nothing the recovery started is still running" \
        "$([ -n "$recovery_group" ] && live_group_members "$recovery_group" | wc -l | tr -d ' ' || echo unknown)" 0
fi

cleanup
trap - EXIT INT TERM
for pidfile in "$WORK"/*.pid; do
    [ -f "$pidfile" ] || continue
    kill -9 "$(cat "$pidfile")" 2>/dev/null || true
done

echo
if [ "$fails" -eq 0 ]; then
    echo "test_watchdogd_shutdown_contract: OK"
else
    echo "test_watchdogd_shutdown_contract: $fails failure(s)"
    echo "--- invocations ---"
    cat "$WORK/invocations"
    exit 1
fi
