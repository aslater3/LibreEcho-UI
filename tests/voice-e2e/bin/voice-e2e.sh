#!/bin/bash
# LibreEcho voice pipeline end-to-end harness (host entry point).
#
#   voice-e2e.sh                     positive fixture, live Wyoming STT, stub LLM
#   voice-e2e.sh --negative          assert the wake word does NOT fire
#   voice-e2e.sh --stt fake          fully hermetic: no network at all
#   voice-e2e.sh --llm live --llm-base-url http://host:port/v1
#   voice-e2e.sh --fixture path.wav --expect 'weather'
#   run-suite.sh [--live]            all cases, with a pass/fail summary
#
# Runs in a native linux/arm64 container (PLATFORM=linux/amd64 also works but
# emulated signal delivery there is broken) against binaries built from the
# working tree by harness/bin/build.sh.  Nothing touches the physical device.
set -euo pipefail
H="$(cd "$(dirname "$0")/.." && pwd)"
UI=${UI:-$(cd "$H/../.." && pwd)}
PLATFORM=${PLATFORM:-linux/arm64}
case "$PLATFORM" in
  linux/arm64) ORTLIB="$H/vendor/onnxruntime-linux-aarch64-1.17.3/lib" ;;
  linux/amd64) ORTLIB="$H/vendor/onnxruntime-linux-x64-1.17.3/lib" ;;
  *) echo "unsupported PLATFORM=$PLATFORM" >&2; exit 2 ;;
esac
IMAGE=libreecho-voice-harness:4

FIXTURE=/harness/fixtures/alexa-what-time.wav
MODE=positive
STT_MODE=live
LLM_MODE=fake
EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        --negative)
            MODE=negative
            FIXTURE=/harness/fixtures/no-wake-word.wav
            shift ;;
        --fixture)   FIXTURE="/harness/fixtures/$(basename "$2")"; shift 2 ;;
        --stt)       STT_MODE=$2; shift 2 ;;
        --llm)       LLM_MODE=$2; shift 2 ;;
        --llm-base-url) EXTRA+=(-e "LLM_BASE_URL=$2"); shift 2 ;;
        --stt-uri)   EXTRA+=(-e "STT_URI=$2"); shift 2 ;;
        --expect)    EXTRA+=(-e "EXPECT_TRANSCRIPT=$2"); shift 2 ;;
        --attenuate) EXTRA+=(-e "ATTENUATE=$2"); shift 2 ;;
        --sensitivity) EXTRA+=(-e "SENSITIVITY=$2"); shift 2 ;;
        --expect-wakes) EXTRA+=(-e "EXPECT_WAKES=$2"); shift 2 ;;
        --listen)       EXTRA+=(-e "LISTEN_SECONDS=$2"); shift 2 ;;
        -e)          EXTRA+=(-e "$2"); shift 2 ;;
        -h|--help)   sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --keep)      KEEP=1; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[ -x "$UI/build/harness/libreecho-waked-onnx" ] || {
    echo "binaries missing -- run $H/bin/build.sh first" >&2; exit 1; }
[ -f "$H/fixtures/$(basename "$FIXTURE")" ] || {
    echo "fixture missing -- run $H/bin/make-fixtures.py first" >&2; exit 1; }

ARGS=(run --rm --platform "$PLATFORM"
      -v "$UI":/work -v "$H":/harness:ro
      -v "$ORTLIB":/ortlib:ro
      -e LD_LIBRARY_PATH=/ortlib
      -e "FIXTURE=$FIXTURE" -e "MODE=$MODE"
      -e "STT_MODE=$STT_MODE" -e "LLM_MODE=$LLM_MODE")
ARGS+=("${EXTRA[@]+"${EXTRA[@]}"}")
ARGS+=(-w /work "$IMAGE" bash /harness/lib/run-in-container.sh)

exec docker "${ARGS[@]}"
