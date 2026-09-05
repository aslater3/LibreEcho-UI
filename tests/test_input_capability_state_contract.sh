#!/bin/sh
set -eu

python3 - <<'PY'
from pathlib import Path
import json
operation = json.loads(Path('web/openapi.json').read_text())['paths']['/buttons']['put']
schema = operation['requestBody']['content']['application/json']['schema']
assert {tuple(case['required']) for case in schema['anyOf']} == {(key,) for key in schema['properties']}
assert '400' in operation['responses'] and '503' in operation['responses']

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
    'buttond detects final action key and legacy PMIC mute':
        'TEST_BIT(KEY_HELP, key_bits)' in buttond and
        'TEST_BIT(KEY_POWER, key_bits)' in buttond and
        'ctx->devices[ctx->device_count].action_capable = action_capable' in buttond and
        'ctx->action_capable |= ctx->devices[i].action_capable' in buttond and
        'action=%d' in buttond,
    'privacy synchronization has a bounded poll interval':
        '#define PRIVACY_POLL_MS 100' in buttond and
        'timeout > PRIVACY_POLL_MS' in buttond and
        'timeout = PRIVACY_POLL_MS;' in buttond,
    'button preferences load before first event':
        buttond.index('refresh_tone_setting(&ctx);') < buttond.index('discover(&ctx);') and
        buttond.index('refresh_tone_setting(&ctx);') < buttond.index('while (!stop_requested)'),
    'buttond stores capability bits per device':
        'struct device' in buttond and
        'ctx->devices[ctx->device_count].volume_capable = volume_capable' in buttond and
        'ctx->devices[ctx->device_count].mute_capable = mute_capable' in buttond,
    'buttond recomputes capabilities after disconnect':
        'static void recompute_capabilities' in buttond and
        'recompute_capabilities(ctx);' in buttond[buttond.index('static void remove_device'):],
    'buttond wakes idle devices for heartbeat':
        'buttond_poll_timeout_ms' in buttond and 'next_status_ms' in buttond,
    'buttond waits for repeat deadline':
        'buttond_repeat_due' in buttond and
        '!ready && buttond_repeat_due' in buttond,
    'buttond rescans without duplicating watched devices':
        'device_path_watched' in buttond and
        'if (ctx.rescan_requested || monotonic_ms() >= next_rescan_ms)' in buttond and
        'if (!ctx.device_count &&' not in buttond,
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
        'tests/test_input_capability_state_contract.sh' in runner and
        'build/test-buttond-timing' in runner,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit('missing issue #68 contract: ' + '; '.join(failed))
print('input capability state contract: ok')
PY
