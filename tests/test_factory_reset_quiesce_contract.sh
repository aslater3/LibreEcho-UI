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
                 'libreecho-agentd.init', 'libreecho-waked.init',
                 'libreecho-micd.init'):
    assert expected in table, expected
# waked is quiesced because it is not a reader: the one-shot dump request has it
# create config/wake-dump.raw inside the reset scope once its startup work is
# done, which can be after the clear has scanned the directory. The list comment
# therefore has to explain it and must not still file every voice daemon under
# "deliberately absent" as reading the configuration only.
comment = source[source.index('* The services that must be stopped'):start]
assert 'waked' in comment and 'wake-dump.raw' in comment
assert 'waked' not in comment.split('deliberately absent')[-1]
# micd and waked are one unit in both directions: micd offers its stream once
# and waked attaches to it once with no reconnect path, so the consumer has to
# be stopped before the producer and started after it.
capture_start = source.index('static const char *const factory_reset_capture[]')
capture = [line for line in source[capture_start:source.index('};', capture_start)].splitlines()
           if '"/etc/init.d/' in line]
assert capture[0].strip().startswith('"/etc/init.d/libreecho-micd.init"'), capture
assert capture[1].strip().startswith('"/etc/init.d/libreecho-waked.init"'), capture
# The stop order is the table order and the restart order is not: the consumer
# is stopped before the producer, and the producer is started before the
# consumer, so the pair is back with the micd the restarted waked attaches to.
assert services.index('    "/etc/init.d/libreecho-waked.init",') < \
    services.index('    "/etc/init.d/libreecho-micd.init"'), services
# The comment says why the order is what it is, and the sentence is matched
# against the comment as prose: it is wrapped in the source, so a line-for-line
# match would break on every reformat.
prose = ' '.join(comment.replace('*', ' ').split())
for expected in ('the reset stops the consumer before the producer and brings '
                 'the pair back as a unit, micd first',
                 'waked connects to the mono stream micd offers once and has no '
                 'reconnect path'):
    assert expected in prose, expected
# The pair is restored as a unit: the generic resume loop skips both members and
# the unit is put back with the producer first.
resume_start = source.index('static void resume_factory_reset_services(')
resume_body = source[resume_start:source.index('\n}\n', resume_start)]
assert 'restore_factory_reset_capture(stopped)' in resume_body
assert 'i == producer || i == consumer' in resume_body
unit_start = source.index('static void restore_factory_reset_capture(')
unit_body = source[unit_start:resume_start]
assert '!stopped[producer] && !stopped[consumer]' in unit_body
assert unit_body.index('factory_reset_services[producer], "start"') < \
    unit_body.index('factory_reset_services[consumer], "start"'), unit_body
assert unit_body.index('factory_reset_services[producer], "stop"') < \
    unit_body.index('factory_reset_services[producer], "start"'), unit_body
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
