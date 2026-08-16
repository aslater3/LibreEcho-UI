#!/bin/sh
set -eu
python3 - <<'PY'
from pathlib import Path

btd = Path('src/adapter/btd.c').read_text()
backend = Path('src/backend.h').read_text()
linux = Path('src/backend_linux.c').read_text()
ui = Path('web/js/bluetooth.js').read_text()

assert 'int rssi_valid;' in btd
assert 'rssi_valid' in backend
assert 'json_get_bool(item, "rssi_valid"' in linux
assert 'update_device_from_eir' in btd
assert 'read_remote_rssi' in btd
assert 'nanosleep(&delay, NULL)' in btd
assert 'refresh_connected_rssi' in btd
assert 'MGMT_EV_DEVICE_CONNECTED' in btd
assert 'device->rssi_valid = 1' in btd
assert 'rssi_valid' in btd
assert 'device.rssi_valid' in ui
assert 'device.rssi ?' not in ui
PY
printf '%s\n' 'bluetooth device metadata contract: ok'
