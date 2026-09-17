#!/bin/sh
set -eu
python3 - <<'PY'
from pathlib import Path
source = Path('src/backend_linux.c').read_text()
assert '"/etc/init.d/libreecho-watchdogd.init"' in source
assert '"/etc/init.d/libreecho-btd.init"' in source
assert '"/etc/init.d/libreecho-ledd.init"' in source
assert '"/etc/init.d/libreecho-networkd.init"' in source
assert '"/etc/init.d/libreecho-timerd.init"' in source
assert '"/etc/init.d/libreecho-agentd.init"' in source
assert '"/etc/init.d/libreecho-waked.init"' in source
start = source.index('static const char *const factory_reset_services[]')
table = source[start:source.index('};', start)]
services = [line for line in table.splitlines() if '"/etc/init.d/' in line]
# The supervisor comes first: it restarts any supervised daemon that stops
# answering, so stopping the writers before it would let it undo the quiesce.
assert services[0].strip().startswith('"/etc/init.d/libreecho-watchdogd.init"'), services
for expected in ('libreecho-btd.init', 'libreecho-ledd.init',
                 'libreecho-networkd.init', 'libreecho-timerd.init',
                 'libreecho-agentd.init', 'libreecho-waked.init'):
    assert expected in table, expected
# waked is quiesced because it is not a reader: the one-shot dump request has it
# create config/wake-dump.raw inside the reset scope once its startup work is
# done, which can be after the clear has scanned the directory. The list comment
# therefore has to explain it and must not still file every voice daemon under
# "deliberately absent" as reading the configuration only.
comment = source[source.index('* The services that must be stopped'):start]
assert 'waked' in comment and 'wake-dump.raw' in comment
assert 'waked' not in comment.split('deliberately absent')[-1]
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
print('factory reset quiesces autonomous persistent-state writers, supervisor first: ok')
PY
