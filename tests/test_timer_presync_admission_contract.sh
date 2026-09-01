#!/bin/sh
set -eu
python3 - <<'PY'
from pathlib import Path
source = Path('src/adapter/timerd.c').read_text()
add = source[source.index('if (!strcmp(cmd, "add"))'):source.index('if (!strcmp(cmd, "add_alarm"))')]
assert 'if (!ctx->state_loaded)' in add
assert 'timer schedule is restoring' in add
print('pre-NTP timer additions wait for persisted schedule restore: ok')
PY
