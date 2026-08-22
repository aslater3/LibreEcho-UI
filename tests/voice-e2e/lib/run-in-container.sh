#!/bin/bash
# Voice pipeline end-to-end harness -- runs INSIDE the harness container.
# Do not invoke directly; use harness/bin/voice-e2e.sh.
#
# Chain under test (all production binaries except the two codec stand-ins):
#
#   WAV fixture
#     -> fake-tinycap        9ch S24_3LE, 16 valid bits left-justified
#     -> libreecho-micd      unpack + calibration + beamform + HPF + gain
#     -> libreecho-waked     openWakeWord alexa_v0.1 via the real ONNX engine
#     -> libreecho-agentd    voice_pipeline: PCM ring, wake -> STT -> LLM
#     -> libreecho-sttd-wyoming -> Wyoming ASR (real or hermetic stub)
#     -> harness-curl / curl -> LLM
#     -> mock-audio-adapter  captures the speak request ttsd would have got
set -uo pipefail

B=/work/build/harness
RUN=/tmp/voice-run
FIXTURE=${FIXTURE:?}
MODE=${MODE:-positive}
STT_MODE=${STT_MODE:-live}
LLM_MODE=${LLM_MODE:-fake}
STT_URI=${STT_URI:-tcp://192.168.69.2:30310}
ATTENUATE=${ATTENUATE:-4}
LEAD_MS=${LEAD_MS:-1500}
# Per-channel room tone.  The array idles at 12-17 RMS post-unpack on
# hardware; ATTENUATE divides what we inject, so inject 15*ATTENUATE.
ROOM_TONE=${ROOM_TONE:-60}
WAKE_TIMEOUT=${WAKE_TIMEOUT:-25}
TURN_TIMEOUT=${TURN_TIMEOUT:-45}
SENSITIVITY=${SENSITIVITY:-}
EXPECT_TRANSCRIPT=${EXPECT_TRANSCRIPT:-time}
LLM_REPLY=${LLM_REPLY:-It is four thirty two in the afternoon.}
LLM_MODEL=${LLM_MODEL:-harness}

rm -rf "$RUN"; mkdir -p "$RUN"
PIDS=()
FAILURES=0
declare -a RESULTS

cleanup() {
    local pid
    for pid in "${PIDS[@]:-}"; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null
    done
    sleep 0.3
    for pid in "${PIDS[@]:-}"; do
        [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null
    done
    wait 2>/dev/null
}
trap cleanup EXIT

stage() {                       # stage <n> <name> <ok:0|1> <detail>
    local n=$1 name=$2 ok=$3 detail=$4
    if [ "$ok" = 0 ]; then
        RESULTS+=("  [PASS]  stage $n  $name -- $detail")
    else
        RESULTS+=("  [FAIL]  stage $n  $name -- $detail")
        FAILURES=$((FAILURES + 1))
    fi
    printf '  %-6s stage %d  %-10s %s\n' \
        "$([ "$ok" = 0 ] && echo '[PASS]' || echo '[FAIL]')" "$n" "$name" "$detail"
}

wait_for() {                    # wait_for <seconds> <shell-test>
    local limit=$1 i
    shift
    for ((i = 0; i < limit * 10; ++i)); do
        if eval "$@"; then return 0; fi
        sleep 0.1
    done
    return 1
}

echo "=== LibreEcho voice pipeline end-to-end ==="
echo "  fixture=$(basename "$FIXTURE")  mode=$MODE  stt=$STT_MODE  llm=$LLM_MODE"
echo

# ---------------------------------------------------------------- fixture prep
python3 - "$FIXTURE" "$RUN/source.raw" <<'PY'
import sys, wave
src, dst = sys.argv[1], sys.argv[2]
with wave.open(src, "rb") as w:
    assert w.getnchannels() == 1, "fixture must be mono"
    assert w.getsampwidth() == 2, "fixture must be S16"
    assert w.getframerate() == 16000, "fixture must be 16 kHz"
    pcm = w.readframes(w.getnframes())
open(dst, "wb").write(pcm)
print("  fixture: %d ms of 16 kHz mono S16" % (len(pcm) // 32))
PY
[ $? -eq 0 ] || { echo "fixture rejected"; exit 1; }

export LE_FAKE_TINYCAP_SOURCE="$RUN/source.raw"
export LE_FAKE_TINYCAP_ATTENUATE="$ATTENUATE"
export LE_FAKE_TINYCAP_LEAD_MS="$LEAD_MS"
export LE_FAKE_TINYCAP_ROOM_TONE_RMS="$ROOM_TONE"
export LE_FAKE_TINYMIX_LOG="$RUN/tinymix.log"
touch "$RUN/pcm-endpoint"           # micd only checks this path is readable

# ------------------------------------------------------------------ hermetic STT
if [ "$STT_MODE" = fake ]; then
    python3 /harness/lib/fake_wyoming_stt.py --port 31310 \
        --transcript "what time is it" --record "$RUN/stt-audio.log" \
        >"$RUN/fake-stt.log" 2>&1 &
    PIDS+=($!)
    STT_URI=tcp://127.0.0.1:31310
    wait_for 10 '[ -s "$RUN/fake-stt.log" ]' || { echo "fake STT did not start"; exit 1; }
fi

# ----------------------------------------------------------------------- micd
"$B/libreecho-micd" --foreground --socket "$RUN/mic.sock" \
    --pcm-path "$RUN/pcm-endpoint" \
    --capture-bin "$B/fake-tinycap" \
    --mixer-bin "$B/fake-tinymix" \
    --idme-dir "$RUN/no-idme" --dt-idme-dir "$RUN/no-idme" \
    >"$RUN/micd.log" 2>&1 &
PIDS+=($!)
wait_for 10 '[ -S "$RUN/mic.sock" ]' || { echo "micd did not come up"; cat "$RUN/micd.log"; exit 1; }

# --------------------------------------------------------- stage 1: capture path
PROBE=$(python3 /harness/lib/mic_probe.py --socket "$RUN/mic.sock" --seconds 1.2 2>&1)
PROBE_OK=$?
if [ $PROBE_OK -ne 0 ]; then
    stage 1 capture 1 "$PROBE"
else
    RMS=$(echo "$PROBE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["rms"])')
    CLIPPED=$(echo "$PROBE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["clipped"])')
    MIXES=$(wc -l <"$RUN/tinymix.log" 2>/dev/null || echo 0)
    # The lead is room tone, so this measures the idle floor.  On hardware that
    # sits at 12-17 RMS post-unpack; with micd's 4x digital gain the mono
    # stream idles around 50-70.  A three-digit idle floor means the 24-bit
    # unpack scaling has regressed (the old bug read 256x too hot).
    if python3 -c "import sys; sys.exit(0 if 5 <= $RMS <= 400 and $CLIPPED == 0 else 1)"; then
        stage 1 capture 0 "idle floor rms=$RMS peak-clipped=$CLIPPED, $MIXES mixer writes, $(echo "$PROBE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["beamforming"])')"
    else
        stage 1 capture 1 "idle floor rms=$RMS clipped=$CLIPPED -- outside the 5..400 the unpack contract implies"
    fi
fi
sleep 0.5    # let micd reap the probe's stream worker

# ---------------------------------------------------------------------- waked
"$B/libreecho-waked-onnx" --foreground --socket "$RUN/wake.sock" \
    --led-socket "$RUN/led.sock" \
    --mic-socket "$RUN/mic.sock" \
    --reference-socket "$RUN/aec-reference.sock" \
    --model-dir /harness/models \
    --wake-threads 1 --vad-floor-rms 16 \
    >"$RUN/waked.log" 2>&1 &
WAKED_PID=$!
PIDS+=($WAKED_PID)
wait_for 30 '[ -S "$RUN/wake.sock" ]' || { echo "waked did not come up"; cat "$RUN/waked.log"; exit 1; }

# ----------------------------------------------------------------------- sttd
LE_STT_WYOMING_URI="$STT_URI" \
LE_STT_WYOMING_MODEL="${STT_MODEL:-whisper-small}" \
LE_STT_END_SILENCE_MS="${END_SILENCE_MS:-1000}" \
LE_STT_MAX_UTTERANCE_MS="${MAX_UTTERANCE_MS:-6000}" \
LE_STT_VAD_FLOOR_RMS="${VAD_FLOOR_RMS:-200}" \
"$B/libreecho-sttd-wyoming" --socket "$RUN/stt.sock" \
    --model-dir "$STT_URI" >"$RUN/sttd.log" 2>&1 &
PIDS+=($!)
wait_for 10 '[ -S "$RUN/stt.sock" ]' || { echo "sttd did not come up"; cat "$RUN/sttd.log"; exit 1; }

# -------------------------------------------------------- tts sink + agent LLM
"$B/mock-audio-adapter" "$RUN/audio.sock" "$RUN/spoken.log" \
    >"$RUN/audio.log" 2>&1 &
PIDS+=($!)
wait_for 10 '[ -S "$RUN/audio.sock" ]' || { echo "audio adapter did not come up"; exit 1; }

if [ "$LLM_MODE" = live ]; then
    # Not bare curl: the tee keeps the request body so stage 3 can still show
    # the transcript the model was actually asked about, in live mode too.
    CURL=/harness/lib/curl-tee.sh
    BASE_URL=${LLM_BASE_URL:?live LLM mode needs LLM_BASE_URL}
else
    CURL="$B/harness-curl"
    BASE_URL=${LLM_BASE_URL:-http://198.51.100.20:8001/v1}
fi
export LE_HARNESS_LLM_LOG="$RUN/llm.log"
export LE_HARNESS_LLM_REPLY="$LLM_REPLY"

# ---------------------------------------------------------------------- agentd
"$B/libreecho-agentd" --socket "$RUN/agent.sock" \
    --config "$RUN/agent.json" --credentials "$RUN/oauth.json" \
    --curl "$CURL" \
    --audio-socket "$RUN/audio.sock" --tts-socket "$RUN/audio.sock" \
    --wake-socket "$RUN/wake.sock" --stt-socket "$RUN/stt.sock" \
    >"$RUN/agentd.log" 2>&1 &
PIDS+=($!)
wait_for 10 '[ -S "$RUN/agent.sock" ]' || { echo "agentd did not come up"; cat "$RUN/agentd.log"; exit 1; }

call() {                        # call <cmd> [args-json]
    python3 - "$RUN/agent.sock" "$1" "${2:-{\}}" <<'PY'
import json, socket, sys
path, cmd, args = sys.argv[1], sys.argv[2], sys.argv[3]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(20)
s.connect(path)
s.sendall(('{"v":1,"id":1,"cmd":"%s","args":%s}\n' % (cmd, args)).encode())
line = b""
while not line.endswith(b"\n"):
    c = s.recv(1)
    if not c:
        break
    line += c
s.close()
sys.stdout.write(line.decode())
PY
}

call configure "{\"provider\":\"openai-compatible\",\"enabled\":true,\"base_url\":\"$BASE_URL\",\"model\":\"$LLM_MODEL\",\"prompt\":\"Answer briefly for spoken playback.\"}" >"$RUN/configure.json"
grep -q '"enabled":true' "$RUN/configure.json" || {
    echo "agentd did not accept the configuration:"; cat "$RUN/configure.json"; exit 1; }

if [ -n "$SENSITIVITY" ]; then
    python3 - "$RUN/wake.sock" "$SENSITIVITY" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(10)
s.connect(sys.argv[1])
s.sendall(('{"v":1,"id":1,"cmd":"set_sensitivity","args":{"sensitivity":%s}}\n'
           % sys.argv[2]).encode())
print("  wake sensitivity set to %s: %s" % (sys.argv[2], s.recv(4096).decode().strip()))
PY
fi

echo "  daemons up; feeding the fixture..."
echo

# -------------------------------------------------------------- stage 2: wake
if [ "$MODE" = negative ]; then
    sleep "$WAKE_TIMEOUT"
    # Stop waked so it prints its metrics summary: "no event" on its own is
    # weak evidence, but "the classifier peaked at 0.002 over N scores" shows
    # the audio really was scored and really did not look like the wake word.
    kill -TERM "$WAKED_PID" 2>/dev/null
    wait_for 5 'grep -q max_wake_score "$RUN/waked.log"' || true
    SUMMARY=$(grep -o "wake_scores=[0-9]* wake_events=[0-9]* max_wake_score=[0-9.]*" "$RUN/waked.log" | tail -1)
    if grep -q "wake_event " "$RUN/waked.log"; then
        stage 2 wake 1 "FALSE POSITIVE: $(grep -m1 "wake_event " "$RUN/waked.log")"
    else
        stage 2 wake 0 "no wake after ${WAKE_TIMEOUT}s -- ${SUMMARY:-no metrics}"
    fi
    echo
    printf '%s\n' "${RESULTS[@]}"
    echo
    [ "$FAILURES" = 0 ] && { echo "RESULT: PASS"; exit 0; } || { echo "RESULT: FAIL ($FAILURES stage(s))"; exit 1; }
fi

if wait_for "$WAKE_TIMEOUT" 'grep -q "wake_event" "$RUN/waked.log"'; then
    WAKE_LINE=$(grep -m1 'wake_event' "$RUN/waked.log")
    stage 2 wake 0 "$(echo "$WAKE_LINE" | sed 's/^waked: //')"
else
    stage 2 wake 1 "no wake_detected within ${WAKE_TIMEOUT}s (see waked.log)"
fi

# ------------------------------------------------- stages 3-5: stt, agent, tts
wait_for "$TURN_TIMEOUT" '[ -s "$RUN/spoken.log" ]' || true
sleep 1
call status >"$RUN/status.json"

TRANSCRIPT=$(python3 - "$RUN/llm.log" <<'PY'
import json, os, re, sys
path = sys.argv[1]
if not os.path.exists(path):
    sys.exit(0)
body = open(path).read()
# openai-compatible request: the transcript is the last user message.
found = re.findall(r'"role"\s*:\s*"user"\s*,\s*"content"\s*:\s*"((?:[^"\\]|\\.)*)"', body)
if found:
    print(json.loads('"%s"' % found[-1]))
PY
)
COMPLETED=$(python3 -c "
import json,sys,re
t=open('$RUN/status.json').read()
m=re.search(r'\"completed_transcripts\":(\d+)',t); print(m.group(1) if m else 0)")
STT_MS=$(python3 -c "
import re
t=open('$RUN/status.json').read()
m=re.search(r'\"last_stt_audio_ms\":(\d+)',t); print(m.group(1) if m else 0)")

MAX_MS=${MAX_UTTERANCE_MS:-6000}
if [ "$COMPLETED" -ge 1 ] && [ -n "$TRANSCRIPT" ]; then
    if [ "$STT_MS" -ge "$MAX_MS" ]; then
        # The turn ran to max_utterance_ms instead of ending on silence.  That
        # is the "VAD latches active and never releases" failure: it still
        # produces a transcript, so only the length gives it away.
        stage 3 stt 1 "\"$TRANSCRIPT\" but the turn ran to the ${MAX_MS} ms cap -- endpointing never fired"
    elif echo "$TRANSCRIPT" | grep -qi "$EXPECT_TRANSCRIPT"; then
        stage 3 stt 0 "\"$TRANSCRIPT\" (${STT_MS} ms of audio, endpointed, $COMPLETED turn(s))"
    else
        stage 3 stt 1 "transcript \"$TRANSCRIPT\" does not match /$EXPECT_TRANSCRIPT/i"
    fi
elif [ "$COMPLETED" -ge 1 ]; then
    stage 3 stt 1 "agentd counted $COMPLETED transcript(s) but no text reached the model"
else
    stage 3 stt 1 "no transcript completed (stt audio_ms=$STT_MS); see sttd.log"
fi

if [ -s "$RUN/llm.log" ] && [ -n "$TRANSCRIPT" ]; then
    CALLS=$(grep -c '' "$RUN/llm.log")
    stage 4 agent 0 "model called with the transcript ($CALLS request(s), provider=openai-compatible)"
elif [ -s "$RUN/llm.log" ]; then
    stage 4 agent 1 "model was called but the request carried no user text"
else
    stage 4 agent 1 "agentd never called the model; see agentd.log"
fi

SPOKEN=$(python3 - "$RUN/spoken.log" <<'PY'
import json, os, re, sys
path = sys.argv[1]
if not os.path.exists(path):
    sys.exit(0)
# agentd streams a long reply as several `speak` calls, so the spoken
# answer is the concatenation, not the last fragment.
parts = []
for line in open(path).read().splitlines():
    m = re.search(r'"text"\s*:\s*"((?:[^"\\]|\\.)*)"', line)
    if m:
        parts.append(json.loads('"%s"' % m.group(1)))
if parts:
    print("".join(parts).strip())
PY
)
if [ -n "$SPOKEN" ]; then
    stage 5 speak 0 "ttsd asked to speak: \"$SPOKEN\""
else
    stage 5 speak 1 "nothing reached the TTS adapter; see agentd.log"
fi

echo
printf '%s\n' "${RESULTS[@]}"
echo
if [ "$FAILURES" = 0 ]; then
    echo "RESULT: PASS"
    exit 0
fi
echo "RESULT: FAIL ($FAILURES stage(s))"
echo
echo "--- waked.log (tail) ---";  tail -8 "$RUN/waked.log"  2>/dev/null
echo "--- sttd.log (tail) ---";   tail -8 "$RUN/sttd.log"   2>/dev/null
echo "--- agentd.log (tail) ---"; tail -12 "$RUN/agentd.log" 2>/dev/null
exit 1
