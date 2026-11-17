#!/bin/sh
# Process finding in init/libreecho-mdnsd.init and init/libreecho-esphomed.init.
#
# A daemon whose file was replaced while it ran (update or hot-staged build)
# shows "<path> (deleted)" as its executable; esphomed stop must still find it.
#
# UI#501: `mdnsd stop` returned 0 while a supervisor whose argv[0] differed
# from $DAEMON (started through a symlink, or otherwise not spelled exactly as
# the wrapper expects) kept running. The wrapper must find the supervisor by
# its resolved executable, as the Platform launcher does, and must not report
# success while one is still running.
#
# UI#502: `esphomed stop` (and so `restart`) did nothing without a pidfile, so
# the old daemon kept port 6053 and the new one could not bind. Stop must find
# the running daemon by its executable when the pidfile is missing.
#
# The stand-in daemons are compiled ELF binaries so argv[0] and the executable
# can be made to differ, which a shell script cannot do. Each stand-in is
# started inside a subshell that reaps it, so a stopped process never lingers as
# a zombie that kill -0 would still count as alive. A bystander process that is
# not the daemon must survive both stops.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
WORK=$ROOT/build/test-init-process-finding
fails=0

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; fails=$((fails + 1)); }
check() { [ "$2" = "$3" ] && pass "$1 (=$3)" || fail "$1: expected $3, got $2"; }

CC=${CC:-$(command -v cc || command -v gcc || command -v clang || true)}
if [ -z "$CC" ]; then
    echo "  SKIP  init process finding checks (no C compiler)"
    exit 0
fi

rm -rf "$WORK"
mkdir -p "$WORK"
bystander=
stubs=
cleanup() {
    for pid in $stubs $bystander; do
        kill -9 "$pid" 2>/dev/null || true
    done
    rm -rf "$WORK"
}
trap cleanup EXIT HUP INT TERM

cat > "$WORK/stub.c" <<'EOC'
#include <unistd.h>
int main(void) { for (;;) pause(); return 0; }
EOC
"$CC" -w -o "$WORK/libreecho-mdnsd" "$WORK/stub.c"
"$CC" -w -o "$WORK/libreecho-esphomed" "$WORK/stub.c"
# A symlink gives the supervisor an argv[0] that is not $DAEMON.
ln -s "$WORK/libreecho-mdnsd" "$WORK/mdnsd-via-symlink"

# Start $1 as a child of a subshell that waits for it, and print its pid.
start_stub() {
    name=$2
    rm -f "$WORK/$name.pid"
    # The subshell must not inherit this function's stdout, or the command
    # substitution that calls start_stub would wait for it indefinitely.
    ( "$1" </dev/null >/dev/null 2>&1 & echo "$!" > "$WORK/$name.pid"; wait ) \
        </dev/null >/dev/null 2>&1 &
    count=0
    while [ ! -s "$WORK/$name.pid" ] && [ "$count" -lt 50 ]; do
        sleep 0.1
        count=$((count + 1))
    done
    cat "$WORK/$name.pid"
}

alive() { kill -0 "$1" 2>/dev/null; }

# ---------------------------------------------------------------- UI#501
echo "UI#501: mdnsd stop finds a supervisor started under another argv[0]"
sup=$(start_stub "$WORK/mdnsd-via-symlink" mdnsd-stub)
stubs="$stubs $sup"
sleep 0.3
exe=$(readlink "/proc/$sup/exe")
argv0=$(tr '\0' '\n' < "/proc/$sup/cmdline" | sed -n '1p')
check "precondition: argv[0] differs from the daemon path" \
    "$([ "$argv0" = "$WORK/libreecho-mdnsd" ] && echo same || echo differs)" differs
check "precondition: executable is the daemon" "$exe" "$WORK/libreecho-mdnsd"

sleep 300 & bystander=$!
rc=0
DAEMON="$WORK/libreecho-mdnsd" PIDFILE="$WORK/mdnsd.pid" LOGFILE="$WORK/mdnsd.log" \
    LOCKDIR="$WORK/mdnsd.lock" ROOT="$WORK/root" SOCKET="$WORK/run/mdns.sock" \
    timeout 60 sh init/libreecho-mdnsd.init stop >"$WORK/mdnsd.out" 2>&1 || rc=$?
check "mdnsd stop exit status" "$rc" 0
check "mdnsd stop left no supervisor running" "$(alive "$sup" && echo yes || echo no)" no
check "mdnsd stop left the unrelated bystander alone" "$(alive "$bystander" && echo yes || echo no)" yes
kill -9 "$bystander" 2>/dev/null || true
bystander=

# ---------------------------------------------------------------- UI#502
echo "UI#502: esphomed stop finds a running daemon without a pidfile"
esp=$(start_stub "$WORK/libreecho-esphomed" esphomed-stub)
stubs="$stubs $esp"
sleep 0.3
check "precondition: no pidfile exists" "$([ -e "$WORK/esphomed.pid" ] && echo yes || echo no)" no
sleep 300 & bystander=$!
rc=0
DAEMON="$WORK/libreecho-esphomed" PIDFILE="$WORK/esphomed.pid" \
    LOGFILE="$WORK/esphomed.log" STATUS_FILE="$WORK/esphomed-status.json" \
    timeout 60 sh init/libreecho-esphomed.init stop >"$WORK/esphomed.out" 2>&1 || rc=$?
check "esphomed stop exit status" "$rc" 0
check "esphomed stop left no daemon running" "$(alive "$esp" && echo yes || echo no)" no
check "esphomed stop left the unrelated bystander alone" "$(alive "$bystander" && echo yes || echo no)" yes
kill -9 "$bystander" 2>/dev/null || true
bystander=

# ------------------------------------------------- replaced while running
echo "esphomed stop finds a daemon whose file was replaced while it ran"
esp=$(start_stub "$WORK/libreecho-esphomed" esphomed-replaced-stub)
stubs="$stubs $esp"
sleep 0.3
"$CC" -w -o "$WORK/libreecho-esphomed.new" "$WORK/stub.c"
mv -f "$WORK/libreecho-esphomed.new" "$WORK/libreecho-esphomed"
check "precondition: the running executable reads as deleted" \
    "$(readlink "/proc/$esp/exe")" "$WORK/libreecho-esphomed (deleted)"
sleep 300 & bystander=$!
rc=0
DAEMON="$WORK/libreecho-esphomed" PIDFILE="$WORK/esphomed.pid" \
    LOGFILE="$WORK/esphomed.log" STATUS_FILE="$WORK/esphomed-status.json" \
    timeout 60 sh init/libreecho-esphomed.init stop >"$WORK/esphomed.out" 2>&1 || rc=$?
check "esphomed stop exit status" "$rc" 0
check "esphomed stop left no replaced daemon running" "$(alive "$esp" && echo yes || echo no)" no
check "esphomed stop left the unrelated bystander alone" "$(alive "$bystander" && echo yes || echo no)" yes
kill -9 "$bystander" 2>/dev/null || true
bystander=

rm -rf "$WORK"
echo
if [ "$fails" -eq 0 ]; then
    echo "test_init_process_finding: OK"
else
    echo "test_init_process_finding: $fails failure(s)"
    exit 1
fi
