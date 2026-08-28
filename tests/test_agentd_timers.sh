#!/bin/sh
# Setting a timer by voice, end to end through the agent.
#
# The parser and the schedule are unit-tested. What this covers is that the
# two are actually joined up: that a spoken request reaches the timer daemon
# and creates a real timer, and -- the part worth proving -- that it does so
# with no language model configured at all. Timers are the thing that should
# keep working when the internet does not.
set -eu

AGENTD=./build/libreecho-agentd
TIMERD=./build/libreecho-timerd
[ -x "$AGENTD" ] || { echo "agentd not built"; exit 1; }
[ -x "$TIMERD" ] || { echo "timerd not built"; exit 1; }

dir=$(mktemp -d)
agent_sock="$dir/agent.sock"
timer_sock="$dir/timer.sock"

cleanup() {
    for name in agentd timerd; do
        [ -f "$dir/$name.pid" ] && kill "$(cat "$dir/$name.pid")" 2>/dev/null || true
    done
    rm -rf "$dir"
}
trap cleanup EXIT INT TERM

call() {
    python3 - "$1" "$2" "$3" <<'PY'
import json, socket, sys
sock, cmd, args = sys.argv[1], sys.argv[2], sys.argv[3]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(10); s.connect(sock)
s.sendall((json.dumps({"v": 1, "id": 1, "cmd": cmd,
                       "args": json.loads(args)}) + "\n").encode())
print(s.recv(65536).decode().strip())
s.close()
PY
}

wait_socket() {
    waited=0
    while [ ! -S "$1" ] && [ "$waited" -lt 10 ]; do sleep 1; waited=$((waited+1)); done
    [ -S "$1" ] || { echo "FAIL: $1 never appeared"; cat "$dir"/*.log 2>/dev/null; exit 1; }
}

"$TIMERD" --foreground --socket "$timer_sock" --state "$dir/timers" \
    --audio-socket "$dir/absent-audio.sock" >"$dir/timerd.log" 2>&1 &
echo $! > "$dir/timerd.pid"
wait_socket "$timer_sock"

# No credentials and no provider: the assistant is not signed in. That is the
# point -- the timer must still work.
printf '{}' > "$dir/agent.json"
"$AGENTD" --socket "$agent_sock" --config "$dir/agent.json" \
    --credentials "$dir/absent-credentials.json" \
    --timer-socket "$timer_sock" \
    --audio-socket "$dir/absent-audio.sock" \
    --tts-socket "$dir/absent-tts.sock" >"$dir/agentd.log" 2>&1 &
echo $! > "$dir/agentd.pid"
wait_socket "$agent_sock"

# --- a spoken request creates a real timer --------------------------------
out=$(call "$agent_sock" respond '{"text":"set a timer for ten minutes"}')
case "$out" in
    *'"ok":true'*) ;;
    *) echo "FAIL: agent rejected the request: $out"; cat "$dir/agentd.log"; exit 1 ;;
esac
case "$out" in
    *'10 minutes'*) ;;
    *) echo "FAIL: confirmation did not mention the duration: $out"; exit 1 ;;
esac
echo "  spoken request answered without a language model: ok"

out=$(call "$timer_sock" status '{}')
# A little time has passed, so check the range rather than an exact second.
printf '%s' "$out" | python3 -c '
import json, sys
data = json.loads(sys.stdin.read())["data"]
timers = data["timers"]
assert len(timers) == 1, "expected one timer, got %r" % (timers,)
assert timers[0]["kind"] == "countdown", timers[0]
left = timers[0]["seconds_remaining"]
assert 540 < left <= 600, "ten minutes should be near 600 seconds, got %d" % left
' || { echo "FAIL: no usable timer was created: $out"; exit 1; }
echo "  the timer daemon actually has the timer: ok"

# --- a question is left for the model -------------------------------------
# The matcher must not swallow things it cannot answer. With no model signed
# in this fails, and failing is correct: it proves the request was passed on
# rather than answered locally.
out=$(call "$agent_sock" respond '{"text":"what is the capital of France"}')
case "$out" in
    *'"ok":false'*) ;;
    *) echo "FAIL: a general question was answered by the timer matcher: $out"; exit 1 ;;
esac
echo "  a general question is still passed to the model: ok"

# --- asking about the timer -----------------------------------------------
out=$(call "$agent_sock" respond '{"text":"how long is left on my timer"}')
case "$out" in
    *'minutes'*) ;;
    *) echo "FAIL: query not answered: $out"; exit 1 ;;
esac
echo "  time remaining answered: ok"

# --- cancelling -----------------------------------------------------------
out=$(call "$agent_sock" respond '{"text":"cancel my timer"}')
case "$out" in
    *'cancelled'*) ;;
    *) echo "FAIL: cancel not confirmed: $out"; exit 1 ;;
esac
out=$(call "$timer_sock" status '{}')
case "$out" in
    *'"timers":[]'*) ;;
    *) echo "FAIL: the timer was not cancelled: $out"; exit 1 ;;
esac
echo "  cancelling by voice removes the timer: ok"

# A duration qualifier is not a timer identity. With one unlabeled timer,
# cancellation must use the single-timer fallback rather than search for
# a label such as "ten minute".
call "$agent_sock" respond '{"text":"set a timer for ten minutes"}' >/dev/null
out=$(call "$agent_sock" respond '{"text":"cancel the ten minute timer"}')
case "$out" in
    *'cancelled'*) ;;
    *) echo "FAIL: duration-only cancellation was not confirmed: $out"; exit 1 ;;
esac
out=$(call "$timer_sock" status '{}')
case "$out" in
    *'"timers":[]'*) ;;
    *) echo "FAIL: duration-only cancellation left the timer scheduled: $out"; exit 1 ;;
esac
echo "  duration-only cancellation uses the single-timer fallback: ok"

# A numeric quantity is not a stored label. With exactly one unlabeled timer,
# the singular quantity must use the same single-timer fallback.
call "$agent_sock" respond '{"text":"set a timer for ten minutes"}' >/dev/null
out=$(call "$agent_sock" respond '{"text":"cancel one timer"}')
case "$out" in
    *'cancelled'*) ;;
    *) echo "FAIL: numeric single-timer cancellation was not confirmed: $out"; exit 1 ;;
esac
out=$(call "$timer_sock" status '{}')
case "$out" in
    *'"timers":[]'*) ;;
    *) echo "FAIL: numeric single-timer cancellation left the timer scheduled: $out"; exit 1 ;;
esac
echo "  numeric single-timer cancellation uses the fallback: ok"

# Singular voice cancellation is label-targeted and must not clear siblings.
call "$agent_sock" respond '{"text":"set a timer for ten minutes for the pasta"}' >/dev/null
call "$agent_sock" respond '{"text":"set a timer for fifteen minutes for the tea"}' >/dev/null
call "$agent_sock" respond '{"text":"set a timer for twenty minutes for the coffee"}' >/dev/null
out=$(call "$agent_sock" respond '{"text":"cancel the pasta timer"}')
case "$out" in
    *'cancelled'*) ;;
    *) echo "FAIL: label cancellation not confirmed: $out"; exit 1 ;;
esac
out=$(call "$timer_sock" status '{}')
case "$out" in
    *'"label":"tea"'*'"timers"'*'"label":"pasta"'*)
        echo "FAIL: label cancellation left pasta or removed tea: $out"; exit 1 ;;
    *'"label":"tea"'*) ;;
    *) echo "FAIL: label cancellation did not preserve the sibling: $out"; exit 1 ;;
esac

# With multiple timers and no identity, do not pick an arbitrary timer.
out=$(call "$agent_sock" respond '{"text":"cancel my timer"}')
case "$out" in
    *'Which timer'*) ;;
    *) echo "FAIL: ambiguous singular cancellation was not rejected: $out"; exit 1 ;;
esac
out=$(call "$timer_sock" status '{}')
case "$out" in
    *'"label":"tea"'*) ;;
    *) echo "FAIL: ambiguous cancellation changed the schedule: $out"; exit 1 ;;
esac

# All accepted cancellation verbs must target their named timer.
out=$(call "$agent_sock" respond '{"text":"delete the tea timer"}')
case "$out" in
    *'cancelled'*) ;;
    *) echo "FAIL: delete label cancellation not confirmed: $out"; exit 1 ;;
esac
out=$(call "$timer_sock" status '{}')
case "$out" in
    *'"label":"tea"'*)
        echo "FAIL: delete label cancellation left tea: $out"; exit 1 ;;
    *'"label":"coffee"'*) ;;
    *) echo "FAIL: delete label cancellation changed the wrong timer: $out"; exit 1 ;;
esac
out=$(call "$agent_sock" respond '{"text":"remove the coffee timer"}')
case "$out" in
    *'cancelled'*) ;;
    *) echo "FAIL: remove label cancellation not confirmed: $out"; exit 1 ;;
esac
out=$(call "$timer_sock" status '{}')
case "$out" in
    *'"timers":[]'*) ;;
    *) echo "FAIL: remove label cancellation left a timer: $out"; exit 1 ;;
esac

# Alarm is an accepted noun as well as timer, and every cancellation verb must
# carry the label through to the daemon.
call "$agent_sock" respond '{"text":"set a timer for ten minutes for the kitchen"}' >/dev/null
out=$(call "$agent_sock" respond '{"text":"clear the kitchen alarm"}')
case "$out" in
    *'cancelled'*) ;;
    *) echo "FAIL: alarm label cancellation not confirmed: $out"; exit 1 ;;
esac
out=$(call "$timer_sock" status '{}')
case "$out" in
    *'"timers":[]'*) ;;
    *) echo "FAIL: alarm label cancellation left a timer: $out"; exit 1 ;;
esac

# Quantifiers only mean cancel-all when they directly quantify a timer/alarm
# noun. They must remain label text (or non-universal wording) elsewhere.
call "$agent_sock" respond '{"text":"set a timer for ten minutes for the all hands"}' >/dev/null
call "$agent_sock" respond '{"text":"set a timer for fifteen minutes for the alpha"}' >/dev/null
call "$agent_sock" respond '{"text":"set a timer for twenty minutes for the beta"}' >/dev/null
out=$(call "$agent_sock" respond '{"text":"cancel the all hands timer"}')
case "$out" in
    *'cancelled'*) ;;
    *) echo "FAIL: quantifier-like label cancellation not confirmed: $out"; exit 1 ;;
esac
out=$(call "$timer_sock" status '{}')
case "$out" in
    *'"label":"alpha"'*'"label":"beta"'*) ;;
    *) echo "FAIL: named quantifier-like label changed the wrong timers: $out"; exit 1 ;;
esac

out=$(call "$agent_sock" respond '{"text":"cancel every question about timers"}')
case "$out" in
    *'could not find that timer'*) ;;
    *) echo "FAIL: unrelated quantifier became cancel-all: $out"; exit 1 ;;
esac
out=$(call "$timer_sock" status '{}')
case "$out" in
    *'"label":"alpha"'*'"label":"beta"'*) ;;
    *) echo "FAIL: unrelated quantifier changed the schedule: $out"; exit 1 ;;
esac

# A quantity is not universal wording and must not clear the schedule.
for phrase in 'cancel one timer' 'cancel two timers'; do
    call "$agent_sock" respond "{\"text\":\"$phrase\"}" >/dev/null
    out=$(call "$timer_sock" status '{}')
    case "$out" in
        *'"label":"alpha"'*'"label":"beta"'*) ;;
        *) echo "FAIL: $phrase was treated as cancel-all: $out"; exit 1 ;;
    esac
done

# The "every one of" construction is universal, even though "one" appears
# between the quantifier and the timer noun.
out=$(call "$agent_sock" respond '{"text":"cancel every one of my timers"}')
case "$out" in
    *'cancelled'*) ;;
    *) echo "FAIL: every-one-of cancel-all not confirmed: $out"; exit 1 ;;
esac
out=$(call "$timer_sock" status '{}')
case "$out" in
    *'"timers":[]'*) ;;
    *) echo "FAIL: every-one-of cancel-all left timers: $out"; exit 1 ;;
esac
call "$agent_sock" respond '{"text":"set a timer for ten minutes"}' >/dev/null
out=$(call "$agent_sock" respond '{"text":"cancel all timers"}')
case "$out" in
    *'cancelled'*) ;;
    *) echo "FAIL: explicit cancel-all not confirmed: $out"; exit 1 ;;
esac
out=$(call "$timer_sock" status '{}')
case "$out" in
    *'"timers":[]'*) ;;
    *) echo "FAIL: explicit cancel-all left timers: $out"; exit 1 ;;
esac
echo "  singular, verb, and universal cancellation are scoped correctly: ok"

# --- "stop" with nothing ringing is not a timer request -------------------
# It means a dozen other things, so it must fall through rather than being
# answered as if a timer had been silenced.
out=$(call "$agent_sock" respond '{"text":"stop"}')
case "$out" in
    *'"ok":false'*) ;;
    *) echo "FAIL: \"stop\" was taken as a timer request with nothing ringing: $out"; exit 1 ;;
esac
echo "  \"stop\" with nothing ringing falls through: ok"

echo "agentd timers: ok"
