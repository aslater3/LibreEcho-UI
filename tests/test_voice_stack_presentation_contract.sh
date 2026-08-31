#!/bin/sh
set -eu

python3 - <<'PY'
from pathlib import Path

ui = Path('web/js/integrations-ui.js').read_text()
assert 'function pipelineStatus(pipeline)' in ui
assert '<dt>Speech path</dt>' in ui
assert 'On-device wakeword, STT and TTS' in ui
assert 'Wyoming STT/TTS endpoints' in ui
assert '<dt>Pipeline mode</dt>' not in ui
print('voice stack presentation contract: ok')
PY
