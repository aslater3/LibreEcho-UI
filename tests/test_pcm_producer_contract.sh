#!/bin/sh
set -eu
# Actual per-stream and framing behaviour lives in the C tests. Keep the
# finite producer integration and duplicated wire contract from drifting.
python3 - <<'PYTHON'
from pathlib import Path
p = Path('src/adapter')
for name in ('audiod.c', 'ttsd.c'):
    text = (p / name).read_text()
    assert 'le_pcm_open(' in text and 'le_pcm_write(' in text, name
    assert 'le_pcm_finish_wait(' in text, name
text = (p / 'live_audio_out.c').read_text()
assert 'cancel system' not in text
assert 'LE_PCM_DRAINED' in text and 'turn_frames_written' in text
print('finite PCM producers use ordered completion; Live never cancels the shared FIFO: PASS')
PYTHON
