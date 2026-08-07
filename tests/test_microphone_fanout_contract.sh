#!/bin/sh
set -eu
python3 - <<'PY'
import json
from pathlib import Path

server = Path('src/http_server.c').read_text()
makefile = Path('Makefile').read_text()
openapi = json.loads(Path('web/openapi.json').read_text())
docs = Path('docs/MICROPHONE_ARCHITECTURE.md').read_text()
assert '#include "adapter/voice_stream.h"' in server
assert 'LE_ADAPTER_WAKEWORD_SOCK' in server
assert 'stream_audio' in server
assert 'le_voice_stream_read_frame' in server
assert 'shared_result!=-2' in server
assert 'src/adapter/voice_stream.c' in makefile
assert 'shared calibrated mono stream' in openapi['paths']['/baby-monitor/stream']['get']['description']
assert 'shared, already-captured mono stream' in docs
print('microphone shared-capture fan-out contract: ok')
PY
