#!/bin/sh
set -eu
python3 - <<'PY'
from pathlib import Path

index = Path('web/index.html').read_text()
source = Path('web/js/bluetooth.js').read_text()

assert '/js/bluetooth.js?rev=29' in index
assert 'scan.disabled=!!b.scanning' in source
assert 'function bluetoothScanFinished(b)' in source
assert 'state.btScanning=!!b.scanning' in source
assert 'bluetoothScanFinished(b);' in source
assert 'Scan complete —' in source

print('bluetooth scan UI and cache-bust contract: ok')
PY
