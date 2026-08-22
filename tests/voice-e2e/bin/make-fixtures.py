#!/usr/bin/env python
"""Build the voice-harness WAV fixtures.

Audio is synthesised on the Wyoming TTS server (piper), resampled to the
16 kHz mono S16_LE the capture path produces, and then *validated* against
the same openWakeWord alexa_v0.1 model waked runs.  A fixture that the
reference model does not score above threshold is rejected rather than
written, so a later harness failure means the pipeline broke -- not that
the fixture was never any good.
"""
import argparse, os, sys, wave
import numpy as np
from scipy.signal import resample_poly

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "lib"))
from wyoming import synthesize

FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fixtures")
POSITIVE = [
    ("en_US-lessac-medium", "Alexa, what time is it?"),
    ("en_US-amy-medium", "Alexa, what time is it?"),
    ("en_US-hfc_female-medium", "Alexa, what time is it?"),
    ("en_GB-alan-medium", "Alexa, what time is it?"),
    ("en_US-ryan-high", "Alexa, what time is it?"),
]
NEGATIVE = ("en_US-lessac-medium",
            "Please turn the kitchen lights off, and lock the back door.")


def to_16k_mono(pcm, rate, width, channels):
    assert width == 2, "expected S16 from piper, got width=%r" % width
    x = np.frombuffer(pcm, dtype="<i2")
    if channels and channels > 1:
        x = x.reshape(-1, channels).mean(axis=1).astype(np.int16)
    if rate != 16000:
        g = np.gcd(int(rate), 16000)
        x = resample_poly(x.astype(np.float64), 16000 // g, int(rate) // g)
        x = np.clip(np.round(x), -32768, 32767).astype(np.int16)
    return x


def pad(x, lead_s=1.0, tail_s=1.0, noise_rms=8):
    """Bracket the phrase with room tone.  openWakeWord needs ~1.4 s of
    context before the word, and a dead-silent lead is not what the mic
    array ever produces."""
    rng = np.random.default_rng(1234)
    lead = rng.normal(0, noise_rms, int(16000 * lead_s))
    tail = rng.normal(0, noise_rms, int(16000 * tail_s))
    out = np.concatenate([lead, x.astype(np.float64), tail])
    return np.clip(np.round(out), -32768, 32767).astype(np.int16)


def rms(x):
    return float(np.sqrt(np.mean(x.astype(np.float64) ** 2)))


def score(x, model_cache={}):
    from openwakeword.model import Model
    m = Model(wakeword_models=["alexa_v0.1"], inference_framework="onnx")
    best, support = 0.0, 0
    for i in range(0, len(x) - 1280, 1280):
        s = m.predict(x[i:i + 1280])["alexa_v0.1"]
        best = max(best, s)
        if s >= 0.35:
            support += 1
    return best, support


def write_wav(path, x):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(x.tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tts", default=os.environ.get("LE_TTS", "192.168.69.2:30320"))
    ap.add_argument("--target-rms", type=float, default=2500.0,
                    help="RMS the spoken part is normalised to")
    args = ap.parse_args()
    host, port = args.tts.split(":")
    port = int(port)
    os.makedirs(FIXTURES, exist_ok=True)

    best = None
    for voice, text in POSITIVE:
        try:
            r, wdt, ch, pcm = synthesize(host, port, text, voice)
        except Exception as e:                       # voice may not exist
            print("  %-28s synth failed: %s" % (voice, e))
            continue
        if not pcm:
            print("  %-28s no audio" % voice)
            continue
        x = to_16k_mono(pcm, r, wdt, ch)
        gain = args.target_rms / max(rms(x), 1.0)
        x = np.clip(np.round(x.astype(np.float64) * gain), -32768, 32767).astype(np.int16)
        y = pad(x)
        peak, support = score(y)
        print("  %-28s %5.2fs rms=%6.0f peak=%.4f support=%d"
              % (voice, len(y) / 16000, rms(x), peak, support))
        if best is None or peak > best[0]:
            best = (peak, support, voice, y)
    if best is None:
        sys.exit("no TTS voice produced audio")
    peak, support, voice, y = best
    if peak < 0.85:
        sys.exit("FIXTURE REJECTED: best peak %.4f (%s) is below the 0.85 "
                 "accept threshold waked uses at sensitivity 0" % (peak, voice))
    write_wav(os.path.join(FIXTURES, "alexa-what-time.wav"), y)
    print("positive fixture: %s peak=%.4f support=%d" % (voice, peak, support))

    voice, text = NEGATIVE
    r, wdt, ch, pcm = synthesize(host, port, text, voice)
    x = to_16k_mono(pcm, r, wdt, ch)
    gain = args.target_rms / max(rms(x), 1.0)
    x = np.clip(np.round(x.astype(np.float64) * gain), -32768, 32767).astype(np.int16)
    y = pad(x)
    peak, support = score(y)
    print("negative fixture: %s peak=%.4f support=%d" % (voice, peak, support))
    if peak >= 0.30:
        sys.exit("FIXTURE REJECTED: negative sample peaks at %.4f, too close "
                 "to the detector floor to prove anything" % peak)
    write_wav(os.path.join(FIXTURES, "no-wake-word.wav"), y)
    print("wrote fixtures to %s" % os.path.abspath(FIXTURES))


if __name__ == "__main__":
    main()
