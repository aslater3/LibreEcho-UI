#!/bin/sh
set -eu
python3 - <<'PY'
from pathlib import Path
source = Path('src/adapter/btd.c').read_text()
init = Path('init/libreecho-btd.init').read_text()
assert '"/etc/libreecho/bluetooth.devices"' not in source
assert '"/etc/libreecho/bluetooth.keys"' not in source
assert '"/data/libreecho/config/bluetooth.devices"' in source
assert '"/data/libreecho/config/bluetooth.keys"' in source
assert 'BT_STATE_DIR=${BT_STATE_DIR:-/data/libreecho/config}' in init
assert '"$BT_STATE_DIR"' in init
print('Bluetooth bond databases are inside the factory-reset root: ok')
PY
