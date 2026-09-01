#!/bin/sh
set -eu
python3 - <<'PY'
from pathlib import Path
source = Path('src/adapter/timerd.c').read_text()
loop = source[source.index('while (!stop_requested) {'):]
step = loop[loop.index('count = le_timer_step'):loop.index('ring_tick(&ctx, now_ms);')]
assert 'unsigned int missed_before = ctx.timers.missed;' in loop
assert 'if (ctx.timers.missed != missed_before)' in step
assert 'ctx.dirty = 1;' in step
print('live missed timers mark the schedule dirty: ok')
PY
