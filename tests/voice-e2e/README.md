# Voice pipeline end-to-end harness

Exercises the real voice path off-device, from the codec byte format up to the
request `ttsd` would have been given:

```
WAV fixture → fake-tinycap (9ch S24_3LE, 16 valid bits left-justified)
  → libreecho-micd           real unpack, calibration, beamform, 80 Hz HPF, 4× digital gain
  → libreecho-waked          real openWakeWord alexa_v0.1 via wake_engine_onnx.cpp
  → libreecho-agentd         real voice_pipeline: PCM ring, wake → STT → LLM
  → libreecho-sttd-wyoming   real Wyoming client → Whisper (live) or a stub
  → mock-audio-adapter       captures the `speak` request
```

Only the two codec tools (`tinycap`/`tinymix`, swapped in via micd's
`--capture-bin`/`--mixer-bin`) and the HTTP/ASR endpoints are stand-ins.
Everything between them is production source, which is the point: it catches
regressions in the code that actually ships.

## Running

```sh
tests/voice-e2e/bin/fetch-ort.sh    # onnxruntime + openWakeWord models, pinned by sha256
tests/voice-e2e/bin/build.sh        # ~40 s, output in build/harness (gitignored)
tests/voice-e2e/bin/run-suite.sh    # hermetic + negative, no network
tests/voice-e2e/bin/run-suite.sh --live   # adds live Whisper and Ollama
```

Exit code is 0/1 and every stage prints its measured evidence, so a failure
names the stage that broke rather than just the outcome.

`--expect-wakes N` plays the whole fixture and asserts how many times the wake
word fired, instead of stopping at the first. The default positive path tears
the daemons down a second after stage 5, so it cannot tell "the second wake
never fired" from "we stopped listening before it was spoken" — which is the
only question that matters when someone reports that only the first Alexa
works. `KEEP_RUN=1` copies the run directory, with every daemon log and
captured request, to `build/harness-run`.

## Why it is worth keeping

Re-introducing the historic 24-bit unpack bug (dropping `value >>= 8` from
`le_voice_unpack_s24_3le`) reproduces the whole documented signature:

```
[FAIL] stage 1 capture  idle floor rms=8333.0 -- outside the 5..400 the unpack contract implies
[PASS] stage 2 wake     score=0.775876          (0.999860 when correct)
[FAIL] stage 3 stt      ran to the 6000 ms cap -- endpointing never fired
```

That is 256× hot capture, a degraded wake margin, and a VAD latched active so
every turn runs to `max_utterance_ms` — the "it keeps listening after I stop
talking" symptom, caught automatically.

## Capture path frequency response (#37 part 3)

```sh
tests/voice-e2e/bin/capture-response.sh          # TOLERANCE=6 by default
```

Plays stepped tones through the real DSP chain and reports magnitude vs
frequency, writing `build/harness/capture-response.json` so builds can be
diffed. It measures the **software** capture chain — unpack scaling,
calibration, beamforming, the 80 Hz high-pass, digital gain — not the acoustic
path, and needs no measurement microphone. The band stops at 8 kHz because
capture runs at 16 kHz; the 20 Hz–20 kHz in #37 describes the output path.

Current baseline is flat to ±0.0 dB from 200 Hz to 7 kHz, with the 80 Hz
high-pass visible below that (−3.0 dB at 80 Hz, −7.0 dB at 40 Hz).

## Notes

- Runs `linux/arm64` by default. Under emulated `linux/amd64` on Apple Silicon
  a caught `SIGTERM` never runs its handler and kills the process (exit 143),
  which makes every daemon look like it crashes on shutdown. That is an
  emulator artifact, not a LibreEcho defect — the same binary is clean on
  native arm64.
- Fixtures are self-validating: `make-fixtures.py` synthesises candidates on a
  Piper server, scores them with reference openWakeWord, and refuses to write a
  positive below 0.85 or a negative above 0.30.

## Not covered

armv7/musl and real MT8163 timing; the device's own wakeword payload (built
from source here instead); AEC and barge-in (`aec_frames=0`); ttsd synthesis
and audio output; follow-up turns, wake cooldown, and noisy-room robustness.
