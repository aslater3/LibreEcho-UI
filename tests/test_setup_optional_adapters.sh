#!/bin/sh
set -eu
python3 - <<'PY'
from pathlib import Path
s=Path('src/api.c').read_text()
needle='if((rc=le_set_wake_word(c->backend,wake))&&rc!=LE_NOT_SUPPORTED)'
needle2='if((rc=le_set_wake_word_sensitivity(c->backend,sensitivity))&&rc!=LE_NOT_SUPPORTED)'
assert needle in s
assert needle2 in s
assert s.index(needle) < s.index('if((rc=le_connect_wifi(c->backend,&wifi))')
print('setup optional voice adapters contract: ok')
PY
