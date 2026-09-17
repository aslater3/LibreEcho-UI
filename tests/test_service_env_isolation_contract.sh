#!/bin/sh
# The service-control environment boundary, as source.
#
# The behaviour is covered by test_service_env_isolation.c (the boundary
# helper) and test_voice_pipeline_env_isolation.c (the real voice-pipeline
# transition under the Web daemon's environment). What those cannot see is the
# next caller: a new exec site that bypasses the boundary, or an init script
# edit that changes the argv the fixture pins. This checks the shipped text for
# both, and that the four names the scripts resolve with ${VAR:-default} are
# the four the boundary drops (issue #249).
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
fails=0

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; fails=$((fails + 1)); }
expect() { [ "$1" = ok ] && pass "$2" || fail "$2"; }
has() { grep -q "$1" "$2" && printf ok || printf no; }
has_fixed() { grep -Fq -- "$1" "$2" && printf ok || printf no; }
lacks() { grep -q "$1" "$2" && printf no || printf ok; }

echo "boundary: every service-control caller uses it"
expect "$(has 'le_service_command(' src/api.c)" \
    "the Web daemon's run_init_command goes through the boundary"
expect "$(lacks 'execl(' src/api.c)" \
    "the Web daemon keeps no raw exec of a service script"
expect "$(has 'le_service_command(' src/backend_linux.c)" \
    "the voice switch and factory reset go through the boundary"
expect "$(lacks 'execl(script' src/backend_linux.c)" \
    "no raw exec of a service script is left in the backend"
expect "$(has 'le_service_command(' src/adapter/watchdogd.c)" \
    "the watchdog recovery path goes through the boundary"
expect "$(lacks 'execl("/bin/sh"' src/adapter/watchdogd.c)" \
    "the watchdog recovery path keeps no raw exec"

echo "boundary: the dropped names are the ones the scripts resolve"
for name in ARGS DAEMON PIDFILE LOGFILE; do
    expect "$(has "\"$name\"" src/service_env.c)" \
        "$name is dropped at the boundary"
done

echo "boundary: the init scripts still resolve their own identity"
# The scripts keep `VAR=${VAR:-default}` -- that rule is what a caller's
# environment used to win. Nothing here may pass another service's values;
# that is the boundary's job, not theirs.
for service in agentd sttd ttsd wyomingd; do
    script="init/libreecho-$service.init"
    for name in ARGS DAEMON PIDFILE LOGFILE; do
        expect "$(has "$name=\${$name:-" "$script")" \
            "$script resolves $name itself"
    done
done

echo "boundary: the fixture pins the shipped argv"
expect "$(has_fixed '--socket $SOCKET --curl $CURL' init/libreecho-agentd.init)" \
    "agentd's own argv"
expect "$(has_fixed '--socket $SOCKET --model-dir $MODEL_ROOT --threads $THREADS' init/libreecho-sttd.init)" \
    "sttd's own argv"
expect "$(has_fixed '--foreground --socket $SOCKET --model-dir $MODEL_DIR --voice $VOICE' init/libreecho-ttsd.init)" \
    "ttsd's own argv"
expect "$(has_fixed '--foreground --port $PORT --wake-socket $WAKE_SOCKET --audio-bus $AUDIO_BUS --mdns-socket $MDNS_SOCKET' init/libreecho-wyomingd.init)" \
    "wyomingd's own argv"
expect "$(has_fixed 'RUNTIME_ROOT=${RUNTIME_ROOT:-/run/libreecho/features/assistant/root}' init/libreecho-agentd.init)" \
    "agentd's runtime root"
expect "$(has_fixed 'RUNTIME_ROOT=${RUNTIME_ROOT:-/run/libreecho/features/stt/root}' init/libreecho-sttd.init)" \
    "sttd's runtime root"
expect "$(has_fixed 'RUNTIME_ROOT=${RUNTIME_ROOT:-/run/libreecho/features/tts/root}' init/libreecho-ttsd.init)" \
    "ttsd's runtime root"
expect "$(has_fixed '/run/libreecho/features/assistant/root/usr/local/sbin/libreecho-agentd' tests/test_voice_pipeline_env_isolation.c)" \
    "the fixture expands agentd's DAEMON the same way"
expect "$(has_fixed '/run/libreecho/features/stt/root/usr/local/sbin/libreecho-sttd' tests/test_voice_pipeline_env_isolation.c)" \
    "the fixture expands sttd's DAEMON the same way"
expect "$(has_fixed '/run/libreecho/features/tts/root/usr/local/sbin/libreecho-ttsd' tests/test_voice_pipeline_env_isolation.c)" \
    "the fixture expands ttsd's DAEMON the same way"
expect "$(has_fixed 'PIDFILE=${PIDFILE:-/var/run/libreecho-wyomingd.pid}' init/libreecho-wyomingd.init)" \
    "wyomingd's pidfile default"
expect "$(has_fixed '/var/run/libreecho-wyomingd.pid' tests/test_voice_pipeline_env_isolation.c)" \
    "the fixture expects wyomingd's own pidfile"

echo
if [ "$fails" -eq 0 ]; then
    echo "test_service_env_isolation_contract: OK"
else
    echo "test_service_env_isolation_contract: $fails failure(s)"
    exit 1
fi
