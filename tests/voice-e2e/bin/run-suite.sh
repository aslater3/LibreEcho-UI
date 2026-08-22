#!/bin/bash
# The regression suite: run every case and summarise.
#
#   run-suite.sh            hermetic + negative (no network needed)
#   run-suite.sh --live     also the live Whisper and live Ollama cases
set -uo pipefail
H="$(cd "$(dirname "$0")/.." && pwd)"
LIVE=0
[ "${1:-}" = --live ] && LIVE=1
STT_URI=${STT_URI:-tcp://192.168.69.2:30310}
LLM_URL=${LLM_URL:-http://192.168.69.2:30434/v1}
LLM_MODEL=${LLM_MODEL:-qwen2.5:3b}

FAIL=0
declare -a LINES
run() {
    local name=$1; shift
    local out
    out=$("$H/bin/voice-e2e.sh" "$@" 2>&1)
    local rc=$?
    if [ $rc -eq 0 ]; then
        LINES+=("PASS  $name")
    else
        LINES+=("FAIL  $name")
        FAIL=$((FAIL + 1))
        printf '%s\n' "$out"
    fi
    printf '%s\n' "$out" | sed -n '/^  \[/p' | sed "s/^/    /"
}

echo "### hermetic: wake + stt stub + llm stub"
run "hermetic positive" --stt fake
echo
echo "### negative: the wake word must not fire"
run "negative" --negative --stt fake
if [ "$LIVE" = 1 ]; then
    echo
    echo "### live Whisper, stub LLM"
    run "live stt" --stt live --stt-uri "$STT_URI"
    echo
    echo "### live Whisper + live Ollama"
    run "live stt + live llm" --stt live --stt-uri "$STT_URI" \
        --llm live --llm-base-url "$LLM_URL" -e "LLM_MODEL=$LLM_MODEL"
fi
echo
echo "===================== suite ====================="
printf '  %s\n' "${LINES[@]}"
[ "$FAIL" = 0 ] && { echo "  ALL PASS"; exit 0; }
echo "  $FAIL case(s) failed"
exit 1
