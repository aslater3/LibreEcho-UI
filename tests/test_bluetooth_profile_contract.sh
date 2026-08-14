#!/bin/sh
set -eu

# Issue #52: kernel protocol modules are not userspace profile services.
# The Bluetooth status contract must report registered profile services
# separately and must not advertise RFCOMM/BNEP/HIDP merely because the kernel
# has those protocol modules enabled.
python3 - <<'PY'
from pathlib import Path

source = Path('src/adapter/btd.c').read_text()
api = Path('src/api.c').read_text()
backend = Path('src/backend.h').read_text()
ui = Path('web/js/bluetooth.js').read_text()

assert 'profile_state' in source
assert 'profile_error' in source
assert 'profile_services' in source
assert '"rfcomm":true,\\"bnep":true,\\"hidp":true' not in source
assert 'profile_state' in api
assert 'profile_services' in api
assert 'profile_state' in backend
assert 'profile_sdp' in backend
assert 'profile_error' in ui
assert 'profile_services' in ui
PY
printf '%s\n' 'bluetooth profile contract: ok'
