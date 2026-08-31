#!/bin/sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
SOURCE=init/libreecho-agentd.init
WORK=$(mktemp -d "${TMPDIR:-/tmp}/libreecho-agentd-ready.XXXXXX")
PRODUCER=0
cleanup() {
    if [ "$PRODUCER" -gt 1 ]; then
        kill "$PRODUCER" 2>/dev/null || true
        wait "$PRODUCER" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

python3 - "$SOURCE" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
assert 'AGENT_DEPENDENCY_TIMEOUT_SECONDS=${AGENT_DEPENDENCY_TIMEOUT_SECONDS:-90}' in source
assert 'AGENT_DEPENDENCY_POLL_SECONDS=${AGENT_DEPENDENCY_POLL_SECONDS:-1}' in source
assert 'dependency_sockets_ready()' in source
assert 'missing_dependency_sockets()' in source
assert 'wait_for_dependency_sockets()' in source
assert 'wait_for_dependency_sockets || {' in source
assert 'dependencies not ready after' in source
for socket_name in ('wakeword.sock', 'stt.sock', 'audio.sock', 'tts.sock'):
    assert socket_name in source
assert 'sleep "$AGENT_DEPENDENCY_POLL_SECONDS"' not in source
assert 'remaining=$((AGENT_DEPENDENCY_TIMEOUT_SECONDS - elapsed))' in source
assert 'poll_seconds=$AGENT_DEPENDENCY_POLL_SECONDS' in source
PY

WAKE_SOCKET=$WORK/wakeword.sock
STT_SOCKET=$WORK/stt.sock
AUDIO_SOCKET=$WORK/audio.sock
TTS_SOCKET=$WORK/tts.sock
AGENT_DEPENDENCY_TIMEOUT_SECONDS=1
AGENT_DEPENDENCY_POLL_SECONDS=2
export WAKE_SOCKET STT_SOCKET AUDIO_SOCKET TTS_SOCKET
export AGENT_DEPENDENCY_TIMEOUT_SECONDS AGENT_DEPENDENCY_POLL_SECONDS

# Extract the actual production functions; do not reimplement their logic in
# the test.  The temporary socket variables above exercise the same paths the
# init script uses during boot.
HELPERS=$(sed -n '/^dependency_sockets_ready()/,/^}/p; /^missing_dependency_sockets()/,/^}/p; /^wait_for_dependency_sockets()/,/^}/p' "$SOURCE")
eval "$HELPERS"

start=$(date +%s)
if wait_for_dependency_sockets 2>"$WORK/timeout.log"; then
    echo 'FAIL: missing dependency sockets were accepted' >&2
    exit 1
fi
elapsed=$(( $(date +%s) - start ))
[ "$elapsed" -le 2 ]
grep -q 'wakeword' "$WORK/timeout.log"
grep -q 'stt' "$WORK/timeout.log"
grep -q 'audio' "$WORK/timeout.log"
grep -q 'tts' "$WORK/timeout.log"

AGENT_DEPENDENCY_TIMEOUT_SECONDS=5
AGENT_DEPENDENCY_POLL_SECONDS=1
export AGENT_DEPENDENCY_TIMEOUT_SECONDS AGENT_DEPENDENCY_POLL_SECONDS
python3 - "$WORK" <<'PY' &
import socket
import sys
import time

root = sys.argv[1]
time.sleep(2)
sockets = []
for name in ('wakeword.sock', 'stt.sock', 'audio.sock', 'tts.sock'):
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(root + '/' + name)
    server.listen(1)
    sockets.append(server)
time.sleep(3)
PY
PRODUCER=$!
start=$(date +%s)
wait_for_dependency_sockets
elapsed=$(( $(date +%s) - start ))
[ "$elapsed" -ge 1 ]
[ -S "$WAKE_SOCKET" ]
[ -S "$STT_SOCKET" ]
[ -S "$AUDIO_SOCKET" ]
[ -S "$TTS_SOCKET" ]
kill "$PRODUCER" 2>/dev/null || true
wait "$PRODUCER" 2>/dev/null || true
PRODUCER=0

echo 'agentd dependency readiness retry contract: ok'
