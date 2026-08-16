#!/bin/sh
set -eu

# Regression contract for issue #73: btd must configure a controller-wide
# DisplayYesNo IO capability before enabling inbound pairing.  This source-level
# check is host-verifiable; the physical iPhone/MGMT capture remains separate.
python3 - <<'PY'
from pathlib import Path

source = Path('src/adapter/btd.c').read_text()
start = source.index('if (!strcmp(command, "set_enabled"))')
end = source.index('if (!strcmp(command, "pairing_mode"))', start)
activation = source[start:end]

assert '#define MGMT_OP_SET_IO_CAPABILITY 0x0018' in source
assert '#define MGMT_IO_CAP_DISPLAY_YES_NO 0x01' in source

powered = activation.index('set_powered(context, 1)')
io_capability = activation.index('controller_command(context, MGMT_OP_SET_IO_CAPABILITY')
bondable = activation.index('set_controller_setting(context, MGMT_OP_SET_BONDABLE, 1)')
assert powered < io_capability < bondable, (
    'controller IO capability must be set after HCI power-on and before '
    'inbound pairing is enabled'
)
assert 'uint8_t io_capability = MGMT_IO_CAP_DISPLAY_YES_NO;' in activation
assert 'sizeof(io_capability)) != 0' in activation
assert 'hci0 IO capability could not be configured' in activation
PY
printf '%s\n' 'bluetooth IO capability contract: ok'
