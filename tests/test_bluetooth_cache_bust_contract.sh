#!/bin/sh
set -eu
python3 - <<'PY'
import re
from pathlib import Path

index = Path('web/index.html').read_text()
source = Path('web/js/bluetooth.js').read_text()

revs=set(re.findall(r'/js/bluetooth\.js\?rev=(\d+)', index))
assert len(revs)==1, f'expected exactly one bluetooth.js revision, got {sorted(revs)}'
rev=int(revs.pop())
assert rev>=29, f'bluetooth.js cache-bust revision went backwards: {rev}'

assert 'disabled=!!b.scanning' in source
assert 'function bluetoothScanFinished(b)' in source
assert 'state.btScanning' in source
assert 'bluetoothScanFinished(b);' in source
assert 'Scan complete —' in source

print(f'bluetooth scan UI and cache-bust contract: ok (rev={rev})')
PY
