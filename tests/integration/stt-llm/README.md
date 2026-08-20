# STT → external LLM integration check

Verifies the LibreEcho voice path end to end with the same wire protocols the
device uses: **Wyoming** for speech-to-text and an **OpenAI-compatible** endpoint
for the LLM. Useful against the emulation or any host running the backends.

## Run

```bash
docker compose up -d
docker compose exec ollama ollama pull qwen2.5:0.5b   # one time

# make a 16 kHz mono WAV of speech (macOS example):
say -o /tmp/q.aiff "What is the capital of France"
afconvert -f WAVE -d LEI16@16000 -c 1 /tmp/q.aiff speech.wav

python3 pipeline.py speech.wav
# STT (Whisper) heard : ' What is the capital of France?'
# LLM reply           : 'The capital of France is Paris.'
```

## Env overrides
`WHISPER_HOST`, `WHISPER_PORT`, `LLM_URL`, `LLM_MODEL` — point at a remote
Whisper (Wyoming) / LLM instead of the local compose stack.
