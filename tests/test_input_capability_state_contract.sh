#!/bin/sh
set -eu

python3 - <<'PY'
from pathlib import Path

buttond = Path('src/adapter/buttond.c').read_text()
api = Path('src/api.c').read_text()
runner = Path('tests/run_tests.sh').read_text()

checks = {
    'buttond writes atomic capability status':
        'STATUS_TMP_PATH' in buttond and 'fsync(fd)' in buttond and
        'rename(STATUS_TMP_PATH, STATUS_PATH)' in buttond,
    'buttond derives capabilities from evdev key bits':
        'volume_capable' in buttond and 'mute_capable' in buttond and
        'TEST_BIT(KEY_MICMUTE, key_bits)' in buttond,
    'buttond reports disconnected/unavailable state':
        'state=%s' in buttond and 'write_capability_status(ctx)' in buttond,
    'API rejects stale capability state':
        'buttond-status' in api and 'stale' in api and 'st_mtime' in api,
    'API no longer hard-codes mute availability':
        '"hardware_mute":true' not in api,
    'API exposes explicit capability fields':
        'volume_capable' in api and 'action_capable' in api and
        'microphone_mute' in api,
    'contract is wired into aggregate runner':
        'tests/test_input_capability_state_contract.sh' in runner,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit('missing issue #68 contract: ' + '; '.join(failed))
print('input capability state contract: ok')
PY
