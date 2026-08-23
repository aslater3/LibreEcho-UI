#!/usr/bin/env python3
from pathlib import Path

source = Path('src/diagnostic_export.c').read_text()

assert '#define LE_DIAG_MAX_BYTES 24576' in source
assert 'O_NOFOLLOW' in source
assert 'fixed_file_summary("/run/libreecho/reset-reason"' in source
assert 'fixed_file_summary("/sys/fs/pstore/console-ramoops-0"' in source
assert 'if (status_rc)' in source and 'append_unavailable(&w, "system")' in source
for subsystem in ('network', 'audio', 'wake_word', 'bluetooth', 'playback'):
    assert f'append_unavailable(&w, "{subsystem}")' in source
assert 'redacted_json(escaped' in source
assert 'wifi_credentials' in source and 'bluetooth_addresses' in source
assert 'network.ip' not in source
assert 'network.ssid' not in source
assert 'bluetooth.known[' not in source
assert 'bluetooth.discovered[' not in source
assert 'device.serial' not in source
print('diagnostic export safety contract: ok')
