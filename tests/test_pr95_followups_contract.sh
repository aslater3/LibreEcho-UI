#!/bin/sh
set -eu
python3 - <<'PY'
from pathlib import Path

server = Path('src/http_server.c').read_text()
frontend = Path('web/js/app.js').read_text()
openapi = Path('web/openapi.json').read_text()
docs = Path('docs/API.md').read_text()

assert 'start_api_worker' in server
assert 'start_api_worker(c->fd,api,&q)' in server
assert 'api_handle(api,q,&r)' in server.split('start_api_worker', 1)[1]
assert 'LE_MAX_ASSISTANT_WORKERS 4' in server
assert 'reap_assistant_workers' in server
assert 'sigaction(SIGCHLD' in server
assert 'access("/etc/init.d/libreecho-sttd.init"' not in server
assert 'access("/etc/init.d/libreecho-ttsd.init"' not in server
assert "Install unsigned update" in frontend
assert "This bypasses signature verification" in frontend
assert 'AllowUnsigned' in openapi
assert 'X-LibreEcho-Allow-Unsigned' in docs
print('PR #95 follow-up contracts: ok')
PY
