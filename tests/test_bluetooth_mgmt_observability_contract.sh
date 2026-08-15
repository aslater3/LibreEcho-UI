#!/bin/sh
set -eu

# Regression contract for issue #62: libreecho-btd must retain and surface the
# MGMT disconnect reason and connect-failed status, and log the MGMT
# connection lifecycle events through the logd channel.
python3 - <<'PY'
from pathlib import Path

source = Path('src/adapter/btd.c').read_text()
parser = Path('src/adapter/bt_mgmt_events.c').read_text()
parser_header = Path('src/adapter/bt_mgmt_events.h').read_text()
backend_h = Path('src/backend.h').read_text()
backend_linux = Path('src/backend_linux.c').read_text()
api = Path('src/api.c').read_text()

# Raw-string JSON key literals exactly as they appear in the C sources
# (backslash-escaped double quotes inside format strings).
daemon_reason_field = r'\"last_disconnect_reason\":\"%s\",'
daemon_status_field = r'\"last_connect_failed_status\":\"%s\",'

# Parser contract: reason/status byte is read at the address-info offset and
# rendered as "name (0xNN)"; undersized payloads are rejected.
assert 'le_bt_mgmt_disconnect_reason_text' in parser_header
assert 'le_bt_mgmt_connect_failed_text' in parser_header
assert 'LE_BT_MGMT_ADDR_INFO_SIZE + 1' in parser
assert 'unrecognized-reason' in parser
assert 'unrecognized-status' in parser

# Event handling: disconnect reason and connect-failed status are retained and
# logged; the pairing LED feedback is unchanged.
assert 'last_disconnect_reason' in source
assert 'last_connect_failed_status' in source
assert 'le_bt_mgmt_disconnect_reason_text(payload, size,' in source
assert 'le_bt_mgmt_connect_failed_text(payload, size,' in source
assert 'MGMT device disconnected: address=' in source
assert 'MGMT connect failed: address=' in source
assert 'MGMT device connected: address=' in source
assert 'MGMT pairing request: address=' in source
assert 'MGMT authentication failed: address=' in source
assert 'led_pattern("flash", 255, 0, 0, 100, 1)' in source

# Status surface: both fields are in the daemon status JSON and flow through
# the backend and API layers.  The daemon and the API serializer emit the
# same backslash-escaped JSON literals inside their C format strings.
assert daemon_reason_field in source
assert daemon_status_field in source
assert 'last_disconnect_reason[48]' in backend_h
assert 'json_get_string(response, "last_disconnect_reason",' in backend_linux
assert 'json_get_string(response, "last_connect_failed_status",' in backend_linux
assert 'disconnect_reason[96]' in api
assert 'json_escape(disconnect_reason,sizeof(disconnect_reason),b.last_disconnect_reason)' in api
assert daemon_reason_field in api
assert daemon_status_field in api
PY
printf '%s\n' 'bluetooth mgmt observability contract: ok'
