#!/bin/sh
set -eu

# Issue #38: the host-verifiable slice is a bounded, explicit contract.  The
# current release intentionally has no upload worker; it must report that
# transport as unavailable rather than pretending to deliver audio.
python3 - <<'PY'
from pathlib import Path

api_h = Path('src/api.h').read_text()
api_c = Path('src/api.c').read_text()
ui = Path('web/js/privacy-ui.js').read_text()
openapi = Path('web/openapi.json').read_text()
docs = Path('docs/API.md').read_text()

checks = {
    'mode is represented separately from the legacy boolean':
        'privacy_audio_mode' in api_h,
    'duration and size are bounded context fields':
        'privacy_audio_retention_hours' in api_h and
        'privacy_audio_max_mb' in api_h,
    'remote endpoint is bounded and stored without credentials':
        'privacy_audio_remote_url' in api_h and
        'audio_remote_username' not in api_h and
        'audio_remote_password' not in api_h,
    'remote mode has an explicit unavailable transport state':
        'remote_retention_transport_available' in api_c and
        'Remote retention transport is unavailable on this release' in api_c,
    'TLS-only endpoint validation exists':
        'https://' in api_c and 'audio_remote_url_valid' in api_c,
    'invalid retention mode is rejected':
        'audio_retention must be none, local, or remote' in api_c,
    'UI exposes destination and bounded controls':
        'audio-retention-destination' in ui and
        'audio_retention_hours' in ui and
        'audio_retention_max_mb' in ui and
        "audio_remote_url:$('#audio-retention-destination').value" in ui,
    'legacy retention flag is local-only for downgrade safety':
        'strcmp(c->privacy_audio_mode,"local")' in api_c,
    'partial URL clearing is checked against effective remote mode':
        'effective_mode' in api_c and 'effective_remote_url' in api_c,
    'Retention panel Save persists log retention':
        "$('#save-retention').onclick=async()=>" in ui and
        "log_retention_hours:parseInt($('#retention').value,10)" in ui,
    'UI surfaces unavailable remote transport':
        'transport unavailable' in ui.lower(),
    'OpenAPI and API docs describe the contract':
        'audio_retention_mode' in openapi and
        'audio_retention_max_mb' in openapi and
        'audio_retention_mode' in docs and
        'audio_retention_max_mb' in docs,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit('missing issue #38 contract: ' + '; '.join(failed))
print('audio retention contract: ok')
PY
