#!/bin/sh
set -eu

# Regression contract for issue #5: an enabled management-controlled HCI
# controller must be made bondable and connectable before an inbound peer can
# generate a pairing request. The source-level assertion is used
# because the real MT8163 HCI/WMT hardware is not available on the host.
python3 - <<'PY'
from pathlib import Path

source = Path('src/adapter/btd.c').read_text()
assert '#define MGMT_OP_SET_LINK_SECURITY 0x000a' in source
assert '#define MGMT_SETTING_LINK_SECURITY 0x00000020U' in source

link_security = source[
    source.index('static int enable_link_security'):
    source.index('static int enable_secure_simple_pairing')
]
assert 'context->supported_settings & MGMT_SETTING_LINK_SECURITY' in link_security
assert 'context->current_settings & MGMT_SETTING_LINK_SECURITY' in link_security
assert 'controller_command(context, MGMT_OP_SET_LINK_SECURITY' in link_security
assert 'context->current_settings |= MGMT_SETTING_LINK_SECURITY;' in link_security

start = source.index('if (!strcmp(command, "set_enabled"))')
end = source.index('if (!strcmp(command, "pairing_mode"))', start)
block = source[start:end]
assert 'set_powered(context, 1)' in block, (
    'enabling Bluetooth must use the Linux management power-on path'
)
assert 'hci_device_up()' not in block, (
    'enabling Bluetooth must not depend on the unsupported legacy HCIDEVUP ioctl'
)
assert '#define HCI_CHANNEL_RAW 0' in source
assert '#define HCIGETCONNINFO' in source
assert 'set_controller_setting(context, MGMT_OP_SET_BONDABLE, 1)' in block, (
    'enabling Bluetooth must set HCI_BONDABLE before an incoming '
    'authentication request'
)
assert 'set_controller_setting(context, MGMT_OP_SET_CONNECTABLE, 1)' in block, (
    'enabling Bluetooth must set HCI_CONNECTABLE before an inbound '
    'connection request'
)
bringup = block[block.index('context->activation_attempted = 1;'):]
assert bringup.index('set_powered(context, 1)') < bringup.index(
    'enable_link_security(context)'
) < bringup.index('enable_secure_simple_pairing(context)'), (
    'BR/EDR link security must be enabled immediately after power-on, before '
    'SSP peers can open protected AVDTP channels'
)
adopted = block[block.index('if (context->enabled)'):block.index(
    'if (context->activation_attempted)'
)]
assert adopted.index('enable_link_security(context)') < adopted.index(
    'enable_secure_simple_pairing(context)'
)
startup = source[source.index('    if (mgmt_open(&context) != 0)'):
                 source.index('    if (le_profile_open', source.index('    if (mgmt_open(&context) != 0)'))]
assert startup.index('enable_link_security(&context)') < startup.index(
    'enable_secure_simple_pairing(&context)'
)
assert 'record_mgmt_status(context, expected, payload[2])' in source
assert 'record_mgmt_io(context, opcode, "response", ETIMEDOUT)' in source
assert 'mgmt_status_name(payload[2])' in source
identity = source[source.index('#define LIBREECHO_BT_MAJOR_CLASS'):source.index('struct sockaddr_hci')]
assert '#define LIBREECHO_BT_MINOR_CLASS 0x14' in identity, (
    'LibreEcho audio output must advertise the loudspeaker class'
)
PY
printf '%s\n' 'bluetooth pairing contract: ok'
