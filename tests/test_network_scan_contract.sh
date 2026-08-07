#!/bin/sh
set -eu
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
assert 'NL80211_BSS_INFORMATION_ELEMENTS' in source
print('network scan EOPNOTSUPP/nl80211 contract: ok')
PY
