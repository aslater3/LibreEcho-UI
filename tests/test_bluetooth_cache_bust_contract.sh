#!/bin/sh
set -eu
python3 - <<'PY'
from pathlib import Path
p=Path('web/index.html').read_text()
assert '/js/bluetooth.js?rev=2' in p
assert '/js/bluetooth.js?rev=1' not in p
print('bluetooth cache-bust contract: ok')
PY
