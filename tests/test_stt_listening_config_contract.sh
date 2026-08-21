#!/bin/sh
set -eu

sh -n init/libreecho-sttd.init
python3 - <<'PY'
from pathlib import Path
import json
init=Path('init/libreecho-sttd.init').read_text()
api=Path('src/api.c').read_text()
stt=Path('src/adapter/stt_engine_wyoming.c').read_text()
openapi=json.loads(Path('web/openapi.json').read_text())
assert 'config_number stt_max_utterance_ms 6000' in init
assert 'export LE_STT_END_SILENCE_MS=$(config_number stt_end_silence_ms 1500)' in init
assert 'export LE_STT_VAD_FLOOR_RMS=$(config_number stt_vad_floor_rms 45)' in init
assert 'config_read(c->config_path, saved' in api
assert 'stt_max_utterance_ms' in api
assert 'stt_end_silence_ms' in api
assert 'stt_vad_floor_rms' in api
assert 'max_utterance = c->stt_max_utterance_ms' in api
assert 'field < 0' in api
assert 'c->stt_max_utterance_ms=max_utterance' in api
assert 'LE_STT_MAX_UTTERANCE_MS", DEFAULT_MAX_UTTERANCE_MS, 2000, 20000' in stt
assert 'VoicePipelineEnvelope' in openapi['paths']['/voice-pipeline']['get']['responses']['200']['content']['application/json']['schema']['$ref']
settings=openapi['components']['schemas']['ListeningSettings']['properties']
assert settings['max_utterance_ms']['maximum']==20000
assert settings['end_silence_ms']['minimum']==200
print('stt listening configuration contract: ok')
PY
