#!/bin/sh
set -eu
python3 - <<'PY'
from pathlib import Path
s = Path('tests/live_readonly_audit.sh').read_text()
assert 'LIBREECHO_LIVE_USERNAME' in s
assert 'LIBREECHO_LIVE_PASSWORD' in s
assert 'LIBREECHO_LIVE_TOKEN' in s
assert 'anonymous_status' in s
assert '= 401 ]' in s
assert 'auth/login' in s
assert 'Authorization: Bearer $TOKEN' in s
assert 'umask 077' in s
assert 'trap cleanup EXIT HUP INT TERM' in s
assert 'set -x' not in s
assert 'password' in s
assert 'read-only checks' in s
print('authenticated live audit contract: ok')
PY
sh -n tests/live_readonly_audit.sh
