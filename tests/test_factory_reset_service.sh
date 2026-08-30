#!/bin/sh
set -eu
python3 - <<'PY'
from pathlib import Path
import json
service = Path('init/libreecho-web.service').read_text()
api = Path('src/api.c').read_text()
openapi = json.loads(Path('web/openapi.json').read_text())
docs = Path('docs/API.md').read_text()
required = [
    'User=libreecho',
    'Group=libreecho',
    'ExecStartPre=+/usr/local/libexec/libreecho-web-migrate-state',
    'Environment=LIBREECHO_DATA_ROOT=/data/libreecho',
    '--config /data/libreecho/config/web-config.json',
    '--users-file /data/libreecho/config/users',
    'ReadWritePaths=/data/libreecho /run/libreecho',
    'CapabilityBoundingSet=CAP_SYS_BOOT CAP_KILL',
    'AmbientCapabilities=CAP_SYS_BOOT CAP_KILL',
    'NoNewPrivileges=true',
    'ProtectSystem=strict',
]
for value in required:
    assert value in service, f'missing hardened factory-reset service contract: {value}'
assert '--config /etc/libreecho/web-config.json' not in service
assert '/var/lib/libreecho' not in service
migrator = Path('init/libreecho-web-migrate-state').read_text()
makefile = Path('Makefile').read_text()
for name in ('web-config.json', 'web-config.json.setup-complete', 'users'):
    assert name in migrator
assert 'LIBREECHO_LEGACY_CONFIG_ROOT' in migrator
assert 'LIBREECHO_DATA_ROOT' in migrator
assert 'test -e $(DESTDIR)/etc/libreecho/web-config.json ||' in makefile
assert 'rc==LE_NOT_SUPPORTED?501:503' in api
for route in ('/system/factory-reset', '/system/reboot', '/system/shutdown'):
    assert {'200', '403', '501', '503'} <= set(openapi['paths'][route]['post']['responses'])
assert 'returns HTTP 503' in docs and 'return HTTP 501' in docs
print('systemd factory-reset state, capability and error contract: ok')
PY
