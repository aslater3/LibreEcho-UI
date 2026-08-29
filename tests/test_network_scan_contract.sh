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
assert '!strncmp(cell->ssid, "NVRAM WARNING:", 14)' in source
assert 'wpa_call(ctx, "ADD_NETWORK", reply' in source
assert 'Wi-Fi password rejected by wpa_supplicant' in source
assert 'static int normalize_rssi_dbm(int rssi_dbm)' in source
assert 'ctx->state.rssi_dbm = normalize_rssi_dbm((int)strtol(value, NULL, 10));' in source
assert 'level = normalize_rssi_dbm((int)statistics.qual.level);' in source
print('network scan EOPNOTSUPP/nl80211 and signed RSSI contract: ok')
PY
