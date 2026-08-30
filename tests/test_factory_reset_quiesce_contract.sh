#!/bin/sh
set -eu
python3 - <<'PY'
from pathlib import Path
source = Path('src/backend_linux.c').read_text()
assert '"/etc/init.d/libreecho-btd.init"' in source
assert '"/etc/init.d/libreecho-timerd.init"' in source
assert '"/etc/init.d/libreecho-agentd.init"' in source
start = source.index('static int factory_reset(')
end = source.index('static const struct le_backend_ops', start)
body = source[start:end]
quiesce = body.index('quiesce_factory_reset_services')
clear = body.index('le_factory_reset_clear')
resume = body.index('resume_factory_reset_services')
reboot = body.index('linux_reboot')
assert 'geteuid() != 0' in body
assert body.index('geteuid() != 0') < quiesce
assert '"/etc/libreecho/bluetooth.devices"' in source
assert '"/etc/libreecho/bluetooth.keys"' in source
assert 'clear_legacy_bluetooth_state' in body
assert 'run_service_action(factory_reset_services[i], "stop") != 0' in source
assert 'run_service_action(factory_reset_services[i], "status") == 0' in source
assert quiesce < clear < reboot
assert clear < resume
assert body.count('resume_factory_reset_services') >= 2
print('factory reset quiesces autonomous persistent-state writers: ok')
PY
