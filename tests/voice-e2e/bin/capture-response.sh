#!/bin/bash
# Measure the capture path's magnitude response (issue #37, part 3).
#
# Only micd is needed: this measures the DSP chain that feeds wake and STT,
# not the acoustic path. See lib/capture_response.py for why the signal is
# stepped tones rather than a continuous sweep.
set -euo pipefail
H="$(cd "$(dirname "$0")/.." && pwd)"
UI=${UI:-$(cd "$H/../.." && pwd)}
PLATFORM=${PLATFORM:-linux/arm64}
IMAGE=libreecho-voice-harness:4
TOLERANCE=${TOLERANCE:-6}
GAIN_TOL=${GAIN_TOL:-5}
THDN_TOL=${THDN_TOL:-1}

[ -x "$UI/build/harness/libreecho-micd" ] || {
    echo "build first: $H/bin/build.sh" >&2; exit 1; }

MODE=${1:-response}
case "$MODE" in
  response)
    python3 "$H/bin/make-sweep.py" \
      --out "$H/fixtures/capture-sweep.wav" --meta "$H/fixtures/capture-sweep.json"
    FIXTURE=capture-sweep ;;
  quality)
    python3 "$H/bin/make-levels.py" \
      --out "$H/fixtures/capture-levels.wav" --meta "$H/fixtures/capture-levels.json"
    FIXTURE=capture-levels ;;
  *) echo "usage: capture-response.sh [response|quality]" >&2; exit 2 ;;
esac

docker run --rm --platform "$PLATFORM" \
    -v "$UI":/work -v "$H":/harness:ro -e FIXTURE="$FIXTURE" -e MODE="$MODE" -e GAIN_TOL="$GAIN_TOL" -e THDN_TOL="$THDN_TOL" -w /work "$IMAGE" \
    bash -euo pipefail -c '
RUN=/tmp/capture-run; rm -rf "$RUN"; mkdir -p "$RUN/no-idme"
B=/work/build/harness
python3 - "/harness/fixtures/$FIXTURE.wav" "$RUN/source.raw" <<"PY"
import sys, wave
w = wave.open(sys.argv[1], "rb")
open(sys.argv[2], "wb").write(w.readframes(w.getnframes()))
PY
export LE_FAKE_TINYCAP_SOURCE="$RUN/source.raw"
export LE_FAKE_TINYCAP_ROOM_TONE_RMS=${ROOM_TONE:-8}
touch "$RUN/pcm-endpoint"           # micd only checks this path is readable
"$B/libreecho-micd" --foreground --socket "$RUN/mic.sock" \
    --pcm-path "$RUN/pcm-endpoint" \
    --capture-bin "$B/fake-tinycap" --mixer-bin "$B/fake-tinymix" \
    --idme-dir "$RUN/no-idme" --dt-idme-dir "$RUN/no-idme" \
    >"$RUN/micd.log" 2>&1 &
for i in $(seq 1 50); do [ -S "$RUN/mic.sock" ] && break; sleep 0.2; done
[ -S "$RUN/mic.sock" ] || { echo "micd did not come up"; cat "$RUN/micd.log"; exit 1; }
# Capture the whole fixture plus a margin
python3 /harness/lib/mic_probe.py --socket "$RUN/mic.sock" --seconds 9 \
    --out "$RUN/captured.raw"
if [ "$MODE" = response ]; then
  python3 /harness/lib/capture_response.py \
      --source "/harness/fixtures/$FIXTURE.wav" \
      --captured "$RUN/captured.raw" \
      --meta "/harness/fixtures/$FIXTURE.json" \
      --tolerance-db '"$TOLERANCE"' \
      --json /work/build/harness/capture-response.json
else
  cp "$RUN/captured.raw" /work/build/harness/capture-levels.raw 2>/dev/null || true
  python3 /harness/lib/capture_quality.py \
      --captured "$RUN/captured.raw" \
      --meta "/harness/fixtures/$FIXTURE.json" \
      --gain-tolerance-pct "$GAIN_TOL" --thdn-tolerance-pct "$THDN_TOL" \
      --json /work/build/harness/capture-quality.json
fi
'
