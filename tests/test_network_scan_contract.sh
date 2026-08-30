#!/bin/sh
set -eu
python3 tests/test_nl80211_scan_completion.py
python3 - <<'PY'
from pathlib import Path
source = Path('src/adapter/networkd.c').read_text()
assert '#include <linux/nl80211.h>' in source
assert 'static int nl80211_scan(' in source
assert 'wext_result == -EOPNOTSUPP' in source
assert 'wext_result == -ENOTSUP' in source
assert 'return scan_errno ? -scan_errno : -EIO;' in source
assert 'NL80211_CMD_TRIGGER_SCAN' in source
assert 'NL80211_CMD_GET_SCAN' in source
assert 'static int nl80211_wait_for_scan_event(' in source
assert 'NL80211_CMD_NEW_SCAN_RESULTS' in source
assert 'NL80211_CMD_SCAN_ABORTED' in source
assert 'nl80211_wait_for_scan_event(fd, buffer, NL80211_BUFFER_SIZE,' in source
assert 'NL80211_SCAN_FRESH_WAIT_MS' not in source
assert 'TRIGGER_SCAN is only an acknowledgement' not in source
assert 'struct scan_result' in source
assert 'static int bogus_ssid(const char *ssid)' in source
assert 'if (bogus_ssid(ssid))' in source
assert 'struct pending_association' in source
assert 'static void check_association(' in source
assert '(void)wpa_ok(ctx, "REMOVE_NETWORK all"' not in source
assert 'Wi-Fi association did not complete' in source
finish_association = source[source.index('static void finish_association'):source.index('static void check_association')]
finish_dhcp = source[source.index('static void finish_dhcp'):source.index('static int start_dhcp')]
assert 'remove_network_profile(ctx, previous)' not in finish_association
assert 'remove_network_profile(ctx, previous)' in finish_dhcp
assert 'else if (!network_success)' in finish_dhcp
assert 'restore_previous_network(ctx);' in finish_dhcp
assert "while (command_length && command[command_length - 1] == '\\n')" in source
assert 'execl("/bin/udhcpc"' in source
assert '"-s", "/etc/udhcpc.script"' in source
assert 'execl("/sbin/udhcpc"' not in source
assert 'LIST_NETWORKS' in source
assert 'DISABLE_NETWORK all' in source
assert 'previous_network_id' in source
assert 'static int normalize_rssi_dbm(int rssi_dbm)' in source
assert 'ctx->state.rssi_dbm = normalize_rssi_dbm((int)strtol(value, NULL, 10));' in source
assert 'level = normalize_rssi_dbm((int)statistics.qual.level);' in source
print('network scan EOPNOTSUPP/nl80211 and signed RSSI contract: ok')
PY
