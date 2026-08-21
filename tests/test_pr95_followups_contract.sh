#!/bin/sh
set -eu
python3 - <<'PY'
from pathlib import Path

server = Path('src/http_server.c').read_text()
frontend = Path('web/js/app.js').read_text()
auth = Path('tests/e2e/tests/auth.setup.ts').read_text()

assert 'start_api_worker' in server
assert 'start_api_worker(c->fd,api,&q)' in server
assert 'api_handle(api,q,&r)' in server.split('start_api_worker', 1)[1]
assert "Install unsigned update" in frontend
assert "This bypasses signature verification" in frontend
assert "fs.chmodSync(AUTH_DIR, 0o700)" in auth
assert "fs.chmodSync(TOKEN_FILE, 0o600)" in auth
assert "fs.chmodSync(STORAGE_FILE, 0o600)" in auth
print('PR #95 follow-up contracts: ok')
PY