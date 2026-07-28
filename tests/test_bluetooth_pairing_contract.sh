#!/bin/sh
set -eu

# Regression contract for issue #5: an enabled management-controlled HCI
# controller must be made bondable and connectable before an inbound peer can
# generate a pairing request. The source-level assertion is used
# because the real MT8163 HCI/WMT hardware is not available on the host.
python3 - <<'PY'
from pathlib import Path

source = Path('src/adapter/btd.c').read_text()
start = source.index('if (!strcmp(command, "set_enabled"))')
end = source.index('if (!strcmp(command, "pairing_mode"))', start)
block = source[start:end]
assert 'hci_device_up()' in block
assert 'set_controller_setting(context, MGMT_OP_SET_BONDABLE, 1)' in block, (
    'enabling Bluetooth must set HCI_BONDABLE before an incoming '
    'authentication request'
)
assert 'set_controller_setting(context, MGMT_OP_SET_CONNECTABLE, 1)' in block, (
    'enabling Bluetooth must set HCI_CONNECTABLE before an inbound '
    'connection request'
)
identity = source[source.index('#define LIBREECHO_BT_MAJOR_CLASS'):source.index('struct sockaddr_hci')]
assert '#define LIBREECHO_BT_MINOR_CLASS 0x14' in identity, (
    'LibreEcho audio output must advertise the loudspeaker class'
)
PY
printf '%s\n' 'bluetooth pairing contract: ok'
