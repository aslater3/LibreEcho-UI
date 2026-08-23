#!/bin/bash
# Wake-word/command pause matrix.
#
# Answers the question behind "I should be able to say the command right after
# Alexa": how long a gap between the wake word and the command can the pipeline
# tolerate before it stops hearing the command at all.
#
# Reports, per gap: whether the wake word fired and at what score, how much
# audio the turn captured, and -- the one that matters -- how much *speech*
# actually reached the recogniser. A turn that captured plenty of audio but no
# command speech is the failure this is looking for.
set -uo pipefail
H="$(cd "$(dirname "$0")/.." && pwd)"
UI=${UI:-$(cd "$H/../.." && pwd)}
GAPS=${GAPS:-0000 0250 0500 1000 1500 2000 3000}

printf '%-8s %-7s %-10s %-9s %-11s %s\n' "gap" "wake" "score" "turn" "cmd audio" "verdict"
printf '%-8s %-7s %-10s %-9s %-11s %s\n' "-----" "----" "-----" "----" "---------" "-------"
for g in $GAPS; do
    out=$(UI="$UI" "$H/bin/voice-e2e.sh" --fixture "pause-$g.wav" \
          --stt fake --llm fake -e KEEP_RUN=1 2>&1)
    run="$UI/build/harness-run"
    score=$(grep -o 'score=[0-9.]*' "$run/waked.log" 2>/dev/null | head -1 | cut -d= -f2)
    ms=$(grep -o 'audio_ms=[0-9]*' "$run/agentd.log" 2>/dev/null | head -1 | cut -d= -f2)
    cmd=$(python3 - "$run/turn-audio.raw" <<'PY'
import struct, sys, math, os
p = sys.argv[1]
if not os.path.exists(p):
    print("-1"); raise SystemExit
raw = open(p, "rb").read(); n = len(raw)//2
if n == 0:
    print("0"); raise SystemExit
v = struct.unpack("<%dh" % n, raw[:n*2])
blk = 160
lv = [math.sqrt(sum(float(x)*x for x in v[i:i+blk])/blk) for i in range(0, n-blk, blk)]
if not lv:
    print("0"); raise SystemExit
gate = max(max(lv)*0.10, 60.0)
print(sum(1 for x in lv if x >= gate) * 10)
PY
)
    wake=$([ -n "$score" ] && echo yes || echo NO)
    if [ "$wake" = NO ]; then verdict="wake missed"
    elif [ "${cmd:-0}" -lt 300 ]; then verdict="COMMAND LOST"
    else verdict="ok"; fi
    printf '%-8s %-7s %-10s %-9s %-11s %s\n' \
      "${g#0} ms" "$wake" "${score:--}" "${ms:--} ms" "${cmd:--} ms" "$verdict"
done
