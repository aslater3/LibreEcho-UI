#!/bin/sh
# Drive the real GPT-Live WebSocket transport end to end.
#
# Usage: run_live_transport_e2e.sh
#
# Starts a stand-in waked and a fake GPT-Live server, points libreecho-lived at
# them, raises a wake and checks what the server actually received. The same
# script runs against a host build or the staged device binary; set
# LIVED_BIN, LIVED_RUN and CONTROL_HOST for the device case.
set -eu
LIVED_BIN=${LIVED_BIN:-./build/libreecho-lived}
WORK=${WORK:-/tmp/live-transport-e2e}
WAKED_SOCKET="$WORK/wakeword.sock"
CONTROL_SOCKET="$WORK/live.sock"
BUS="$WORK/system.pcm"
CREDENTIALS="$WORK/openai-codex.json"
FAKE_LOG="$WORK/fake-server.log"
WAKED_LOG="$WORK/fake-waked.log"
PORT=${PORT:-19301}
LIVED_RUN=${LIVED_RUN:-}       # e.g. "adb -s SERIAL shell"
CONTROL_HOST=${CONTROL_HOST:-127.0.0.1}
# Reap servers from an earlier run: a stale listener keeps the port and the
# new server silently fails to bind, which then looks like a transport fault.
pkill -f "tests/fake_live_server.py" 2>/dev/null || true
pkill -f "tests/fake_waked.py" 2>/dev/null || true
sleep 0.3
rm -rf "$WORK"
mkdir -p "$WORK"
: > "$FAKE_LOG"
: > "$BUS"
# A synthetic credential file. It is never sent to OpenAI: the fake server is
# the only peer, and the point is to exercise the real credential path.
printf '{"access_token":"synthetic-access-token-for-e2e","refresh_token":"r",' > "$CREDENTIALS"
printf '"account_id":"acct-e2e","expires_at":0}\n' >> "$CREDENTIALS"
chmod 600 "$CREDENTIALS"
python3 tests/fake_waked.py "$WAKED_SOCKET" 2 > "$WAKED_LOG" 2>&1 &
WAKED_PID=$!
python3 tests/fake_live_server.py "$PORT" 45 "$FAKE_LOG" > "$WORK/fake.out" 2>&1 &
FAKE_PID=$!
trap 'kill $WAKED_PID $FAKE_PID 2>/dev/null || true' EXIT INT TERM
# Wait for both listeners.
i=0
while [ "$i" -lt 100 ]; do
    [ -S "$WAKED_SOCKET" ] && grep -q FAKE_LISTENING "$WORK/fake.out" && break
    i=$((i + 1)); sleep 0.1
done
if ! grep -q FAKE_LISTENING "$WORK/fake.out"; then
    echo "fake server did not start:"; cat "$WORK/fake.out"; exit 1
fi
if [ -n "$LIVED_RUN" ]; then
    # Device: land the fixtures there too, over loopback via adb reverse.
    $LIVED_RUN "mkdir -p $WORK"
    adb -s "${ADB_SERIAL:?}" push "$CREDENTIALS" "$CREDENTIALS" >/dev/null
    adb -s "$ADB_SERIAL" push "$BUS" "$BUS" >/dev/null
    $LIVED_RUN "for e in /proc/[0-9]*; do a=\$(tr '\\0' '\\n' < \$e/cmdline 2>/dev/null | sed -n 1p); \
        [ \"\$a\" = \"$LIVED_BIN\" ] && kill -TERM \${e#/proc/}; done; sleep 0.5; \
        setsid $LIVED_BIN --foreground --transport realtime \
        --live-url ws://127.0.0.1:$PORT/v1/realtime?intent=quicksilver \
        --credentials $CREDENTIALS --socket $CONTROL_SOCKET \
        --wake-socket $WAKED_SOCKET --audio-bus $BUS \
        --conversation-timeout-ms 4000 >> $WORK/lived.log 2>&1 </dev/null &"
    adb -s "$ADB_SERIAL" forward "tcp:19399" "localfilesystem:$CONTROL_SOCKET" >/dev/null
    CONTROL_PORT=19399
else
    "$LIVED_BIN" --foreground --transport realtime \
        --live-url "ws://127.0.0.1:$PORT/v1/realtime?intent=quicksilver" \
        --credentials "$CREDENTIALS" --socket "$CONTROL_SOCKET" \
        --wake-socket "$WAKED_SOCKET" --audio-bus "$BUS" \
        --conversation-timeout-ms 4000 > "$WORK/lived.log" 2>&1 &
    LIVED_PID=$!
    CONTROL_PORT=0
fi
CONTROL_PORT=${CONTROL_PORT:-0}
echo "--- lived log ---"
sleep 1
cat "$WORK/lived.log" 2>/dev/null || true
echo "--- waiting for the conversation ---"
sleep 6
if [ "$CONTROL_PORT" -gt 0 ]; then
    STATUS=$(python3 tests/live_status.py "$CONTROL_PORT" || true)
else
    STATUS=$(python3 tests/live_status.py "$CONTROL_SOCKET" || true)
fi
echo "--- status ---"
echo "$STATUS"
echo "--- fake server log ---"
cat "$FAKE_LOG"
echo "--- assertions ---"
FAIL=0
check() { if echo "$1" | grep -q "$2"; then echo "  PASS  $3"; else echo "  FAIL  $3"; FAIL=1; fi; }
check "$(cat "$FAKE_LOG")" "SESSION_UPDATE model=gpt-live-1-codex" "session.update carried the model"
check "$(cat "$FAKE_LOG")" "delegation=client" "delegation is client-managed"
check "$(cat "$FAKE_LOG")" "AUDIO_APPEND" "post-AEC audio reached the server"
check "$(cat "$FAKE_LOG")" "DELEGATION_REPLY id=delegation-1" "the delegation was answered"
check "$STATUS" '"messages_in"' "messages were parsed"
check "$STATUS" '"audio_chunks_out"' "audio chunks were sent"
check "$STATUS" '"frames_written"' "model audio reached the playback bus"
check "$STATUS" '"delegations":1' "exactly one delegation was dispatched"
if [ "$FAIL" -eq 0 ]; then echo "live transport e2e: ok"; else echo "live transport e2e: FAILED"; exit 1; fi