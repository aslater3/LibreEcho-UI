#!/bin/sh
set -eu

python3 - <<'PY'
from pathlib import Path

init = Path('init/libreecho-ttsd.init').read_text()
assert 'export LE_TTS_IN_PROCESS=${LE_TTS_IN_PROCESS:-0}' in init, (
    'production init must keep TTS cancellable by default'
)
assert 'export LE_TTS_IN_PROCESS=${LE_TTS_IN_PROCESS:-1}' not in init, (
    'production init must not force blocking in-process synthesis'
)
print('tts production process mode contract: ok')
PY
