#!/bin/sh
set -eu

# Regression contract for issue #73: btd must configure a controller-wide
# DisplayYesNo IO capability before enabling inbound pairing.  This source-level
# check is host-verifiable; the physical iPhone/MGMT capture remains separate.
python3 - <<'PY'
from pathlib import Path

source = Path('src/adapter/btd.c').read_text()
decoder = Path('src/adapter/bt_pairing_events.c').read_text()
start = source.index('if (!strcmp(command, "set_enabled"))')
end = source.index('if (!strcmp(command, "pairing_mode"))', start)
activation = source[start:end]

assert '#define MGMT_OP_SET_IO_CAPABILITY 0x0018' in source
assert '#define MGMT_IO_CAP_DISPLAY_YES_NO 0x01' in source

bringup = activation[activation.index('context->activation_attempted = 1;'):]
powered = bringup.index('set_powered(context, 1)')
io_capability = bringup.index('controller_command(context, MGMT_OP_SET_IO_CAPABILITY')
bondable = bringup.index('set_controller_setting(context, MGMT_OP_SET_BONDABLE, 1)')
assert powered < io_capability < bondable, (
    'controller IO capability must be set after HCI power-on and before '
    'inbound pairing is enabled'
)
assert 'uint8_t io_capability = MGMT_IO_CAP_DISPLAY_YES_NO;' in activation
assert 'sizeof(io_capability)) != 0' in activation
assert 'hci0 IO capability could not be configured' in activation
io_failure = activation[activation.index('controller_command(context, MGMT_OP_SET_IO_CAPABILITY'):activation.index('/* Opening the management channel', activation.index('controller_command(context, MGMT_OP_SET_IO_CAPABILITY'))]
assert '(void)set_powered(context, 0);' in io_failure
assert 'context->enabled = 0;' in io_failure
assert 'context->capability_ready = 0;' in io_failure
assert 'context->capability_ready = 1;' in activation
pairing_mode = source[source.index('static int set_pairing_mode'):source.index('static int handle_request')]
assert 'if (!context->enabled)' in pairing_mode
assert 'controller_command(context, MGMT_OP_SET_IO_CAPABILITY' in pairing_mode
assert 'context->capability_ready = 1;' in pairing_mode
assert pairing_mode.index('if (enabled == context->pairing_mode)') < pairing_mode.index('controller_command(context, MGMT_OP_SET_IO_CAPABILITY')
adopted_powered = activation[activation.index('if (context->enabled)'):activation.index('if (context->activation_attempted)')]
assert 'controller_command(context, MGMT_OP_SET_IO_CAPABILITY' in adopted_powered
assert 'context->capability_ready = 1;' in adopted_powered
assert '(void)set_powered(context, 0);' in adopted_powered

# USER_CONFIRM_REQUEST has address + type + confirmation hint + value; the
# passkey notification layout has address + type + value.  Keep both offsets
# explicit so numeric comparison is not shifted by the hint byte.
assert 'le_bt_pairing_event_value(event, payload, size, &value)' in source
assert 'if (size >= 11)' not in source[source.index('case MGMT_EV_USER_CONFIRM_REQUEST'):source.index('case MGMT_EV_AUTH_FAILED')]
assert 'event == LE_BT_MGMT_EV_USER_CONFIRM_REQUEST ? 8U : 7U' in decoder
assert 'size_t required = offset + sizeof(uint32_t);' in decoder
assert 'if (!payload || !value || size < required)' in decoder

# The executable fixture in tests/test_bt_pairing_events.c feeds complete and
# truncated confirmation/notification payloads through the same C decoder.
assert 'tests/test_bt_pairing_events.c' in Path('Makefile').read_text()
assert 'build/test-bt-pairing-events' in Path('tests/run_tests.sh').read_text()
frontend = Path('web/js/bluetooth.js').read_text()
assert "String(numeric).padStart(6,'0')" in frontend
assert "p.method==='confirm' && p.value !== undefined && p.value !== null" in frontend
assert 'comparisonValue' in frontend
startup = source[source.index('    if (mgmt_open(&context) != 0)'):source.index('    if (le_profile_open', source.index('    if (mgmt_open(&context) != 0)'))]
assert 'refresh_info(&context)' in startup
assert 'if (mgmt_open(&context) != 0)' in startup
assert 'if (refresh_info(&context) != 0 && access(HCI_DEVICE, F_OK) == 0)' in startup
assert 'close(listener);' in startup
assert 'controller_command(&context, MGMT_OP_SET_IO_CAPABILITY' in startup
assert 'context.capability_ready = 1;' in startup
PY
printf '%s\n' 'bluetooth IO capability contract: ok'
