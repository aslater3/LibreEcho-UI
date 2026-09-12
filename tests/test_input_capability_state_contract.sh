#!/bin/sh
set -eu

python3 - <<'PY'
from pathlib import Path
import json
operation = json.loads(Path('web/openapi.json').read_text())['paths']['/buttons']['put']
schema = operation['requestBody']['content']['application/json']['schema']
schema_checks = {
    'schema requires a recognized button field':
        {tuple(case['required']) for case in schema.get('anyOf', [])} ==
        {(key,) for key in schema['properties']} and schema.get('minProperties') == 1,
    'schema preserves target action choices':
        set(schema['properties']['action']['enum']) == {'sound', 'listen', 'playpause', 'disabled'},
    'schema documents validation and persistence failures':
        '400' in operation['responses'] and '503' in operation['responses'],
}

buttond = Path('src/adapter/buttond.c').read_text()
api = Path('src/api.c').read_text()
runner = Path('tests/run_tests.sh').read_text()
button_fixtures = [Path(path).read_text() for path in
                   ('tests/test_buttond_privacy.c', 'tests/test_buttond_events.c')]
fixture_header = Path('tests/buttond_fixture.h')
fixture_source = fixture_header.read_text() if fixture_header.exists() else ''
button_update = api.split('if(!strcmp(p,"/api/v1/buttons")){if(!strcmp(q->method,"PUT"))', 1)[1].split('buttons_json(c,r);return;}', 1)[0]
recognized_guard = 'if(!(short_field>0||long_field>0||tones_field>0||action_field>0||sounds_field>0||action_brightness_field>0||mute_brightness_field>0))'

checks = {
    **schema_checks,
    'API checks for a recognized field before changing preferences':
        recognized_guard in button_update and
        button_update.index(recognized_guard) < button_update.index('if(short_field>0)strcpy'),
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
    'button fixtures intercept hardware opens and assert interception':
        all('#include "buttond_fixture.h"' in test and
            '#define open buttond_fixture_open' in test and
            '#undef open' in test and
            'assert(buttond_fixture_opens > 0);' in test
            for test in button_fixtures),
    'button fixture uses anonymous files for sysfs and denies device nodes':
        'strncmp(path, "/sys/", 5)' in fixture_source and
        'tmpfile()' in fixture_source and
        'strncmp(path, "/dev/", 5)' in fixture_source and
        'errno = ENODEV;' in fixture_source,
    'contract is wired into aggregate runner':
        'tests/test_input_capability_state_contract.sh' in runner and
        'build/test-buttond-timing' in runner,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit('missing issue #68 contract: ' + '; '.join(failed))
print('input capability state contract: ok')
PY
