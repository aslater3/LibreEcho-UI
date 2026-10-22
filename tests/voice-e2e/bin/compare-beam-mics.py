#!/usr/bin/env python3
"""Compare microphone subsets for the micd fixed-beam front end.

Question under test: does feeding all seven microphones to the beamformer
beat the four the stock speech front end uses (lanes 0, 1, 3, 4)?

Each case renders a talker (the alexa-what-time fixture) and, optionally, an
interfering source (the synthetic music bed) through fake-tinycap, which
applies the measured array geometry: per-microphone fractional propagation
delays for far-field plane waves, plus independent self-noise on every lane.
The nine-lane capture then goes through beam-eval, which runs micd's own
decode, beamformer and high-pass code for a given microphone subset.

The figure of merit is SNR improvement over the centre microphone alone:

    SNR = (P(speech windows) - P(noise windows)) / P(noise windows)

measured on the processed output, with speech windows taken from the clean
fixture's envelope.  Positive means the subset beats a single microphone.

Runs natively (gcc, numpy-free) and needs no Docker or device.

    compare-beam-mics.py [--json out.json]
"""
import argparse, array, json, math, os, subprocess, sys, tempfile, wave

H = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = os.path.dirname(os.path.dirname(H))
RATE = 16000
LATENCY = 256          # beamformer STFT delay, samples
SUBSETS = [("4 mics (stock 0,1,3,4)", "0,1,3,4"),
           ("6 ring mics", "0,1,2,3,4,5"),
           ("7 mics (all)", "all")]
REFERENCE = "6"        # centre microphone alone

CASES = [
    # name, talker azimuth, noise azimuth or None, noise gain %, room tone rms
    ("music from 90 deg", 0, 90, 100, 14),
    ("music from 180 deg", 0, 180, 100, 14),
    ("music from 240 deg", 0, 240, 100, 14),
    ("talker between beams, music 120 deg", 15, 120, 100, 14),
    ("self-noise only (no interferer)", 0, None, 0, 400),
]


def build(tmp):
    cc = ["gcc", "-O2", "-std=c99", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
          "-I" + os.path.join(UI, "src"), "-I" + os.path.join(UI, "src", "adapter")]
    fake = os.path.join(tmp, "fake-tinycap")
    beval = os.path.join(tmp, "beam-eval")
    subprocess.run(cc + [os.path.join(H, "src", "fake-tinycap.c"),
                         os.path.join(UI, "src", "adapter", "mic_array_render.c"),
                         "-lm", "-o", fake], check=True)
    subprocess.run(cc + [os.path.join(H, "src", "beam-eval.c"),
                         os.path.join(UI, "src", "adapter", "mic_beamformer.c"),
                         os.path.join(UI, "src", "adapter", "voice_dsp.c"),
                         "-lm", "-o", beval], check=True)
    return fake, beval


def read_wav(path):
    with wave.open(path) as w:
        assert w.getframerate() == RATE and w.getnchannels() == 1 \
            and w.getsampwidth() == 2, path
        return array.array("h", w.readframes(w.getnframes()))


def write_raw(path, samples):
    with open(path, "wb") as f:
        samples.tofile(f)


def render(fake, talker_raw, noise_raw, talker_az, noise_az, noise_gain, tone):
    env = dict(os.environ,
               LE_FAKE_TINYCAP_SOURCE=talker_raw,
               LE_FAKE_TINYCAP_LEAD_MS="3000",
               LE_FAKE_TINYCAP_OFFLINE="1",
               LE_FAKE_TINYCAP_ROOM_TONE_RMS=str(tone),
               LE_FAKE_TINYCAP_AZIMUTH=str(talker_az))
    if noise_az is not None:
        env.update(LE_FAKE_TINYCAP_NOISE_SOURCE=noise_raw,
                   LE_FAKE_TINYCAP_NOISE_AZIMUTH=str(noise_az),
                   LE_FAKE_TINYCAP_NOISE_GAIN=str(noise_gain))
    return subprocess.run([fake, "-c", "9", "-r", "16000", "-b", "24"],
                          env=env, stdout=subprocess.PIPE, check=True).stdout


def process(beval, capture, mics, lock=None):
    env = dict(os.environ, BEAM_EVAL_TRACE="1")
    if lock is not None:
        env["BEAM_EVAL_LOCK_AZIMUTH"] = str(lock)
    run = subprocess.run([beval, mics], input=capture, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, env=env, check=True)
    picks = [int(line.split()[1]) for line in run.stderr.decode().splitlines()
             if line.strip()]
    return array.array("h", run.stdout), picks


def speech_mask(clean, lead, total, latency):
    """Per-20 ms window: True = speech, False = noise-only, None = skip."""
    win = RATE // 50
    env = []
    for start in range(0, len(clean), win):
        seg = clean[start:start + win]
        env.append(math.sqrt(sum(x * x for x in seg) / max(len(seg), 1)))
    peak = max(env)
    mask = [None] * (total // win)
    for i, e in enumerate(env):
        j = (lead + latency) // win + i
        if j < len(mask):
            mask[j] = e > peak * 0.1 if (e > peak * 0.1 or e < peak * 0.01) else None
    # Noise-only windows also come from the lead (after 0.5 s settling).
    for j in range((RATE // 2) // win, (lead + latency) // win):
        mask[j] = False
    return mask


def snr_db(signal, mask):
    win = RATE // 50
    ps, pn = [], []
    for j, m in enumerate(mask):
        if m is None:
            continue
        seg = signal[j * win:(j + 1) * win]
        if len(seg) < win:
            continue
        p = sum(x * x for x in seg) / win
        (ps if m else pn).append(p)
    s, n = sum(ps) / len(ps), sum(pn) / len(pn)
    return 10 * math.log10(max(s - n, 1e-9) / n)


def main():
    ap = argparse.ArgumentParser(description="Compare microphone subsets for the micd beamformer.")
    ap.add_argument("--json")
    args = ap.parse_args()
    clean = read_wav(os.path.join(H, "fixtures", "alexa-what-time.wav"))
    bed = read_wav(os.path.join(H, "fixtures", "music-bedonly.wav"))
    lead = 3 * RATE  # LE_FAKE_TINYCAP_LEAD_MS=3000
    results = []
    with tempfile.TemporaryDirectory() as tmp:
        fake, beval = build(tmp)
        talker_raw = os.path.join(tmp, "talker.raw")
        noise_raw = os.path.join(tmp, "noise.raw")
        write_raw(talker_raw, clean)
        write_raw(noise_raw, bed)
        for name, taz, naz, ngain, tone in CASES:
            capture = render(fake, talker_raw, noise_raw, taz, naz, ngain, tone)
            ref, _ = process(beval, capture, REFERENCE)
            mask_ref = speech_mask(clean, lead, len(ref), 0)
            ref_snr = snr_db(ref, mask_ref)
            row = {"case": name, "talker_azimuth": taz, "noise_azimuth": naz,
                   "centre_mic_snr_db": round(ref_snr, 2),
                   "locked_gain_db": {}, "auto_gain_db": {}, "auto_beam": {}}
            for label, mics in SUBSETS:
                out, _ = process(beval, capture, mics, lock=taz)
                mask = speech_mask(clean, lead, len(out), LATENCY)
                row["locked_gain_db"][label] = round(snr_db(out, mask) - ref_snr, 2)
                out, picks = process(beval, capture, mics)
                row["auto_gain_db"][label] = round(snr_db(out, mask) - ref_snr, 2)
                # Most common beam during the utterance (after the lead).
                tail = picks[(lead + LATENCY) // 128:] or picks
                row["auto_beam"][label] = max(set(tail), key=tail.count)
            results.append(row)
    labels = [label for label, _ in SUBSETS]
    width = max(len(r["case"]) for r in results)
    for key, title in (("locked_gain_db", "beam locked on the talker"),
                       ("auto_gain_db", "automatic beam selection")):
        print(f"\n== SNR gain over centre mic, {title} (dB) ==")
        print(f"{'case':<{width}}  {'centre':>6}  " +
              "  ".join(f"{l:>22}" for l in labels))
        for r in results:
            cells = []
            for l in labels:
                cell = f"{r[key][l]:+.1f}"
                if key == "auto_gain_db":
                    cell += f" @{r['auto_beam'][l]}"
                cells.append(f"{cell:>22}")
            print(f"{r['case']:<{width}}  {r['centre_mic_snr_db']:>6.1f}  " +
                  "  ".join(cells))
    print("\nGain = output SNR minus centre-microphone SNR.  @n = beam azimuth"
          " auto selection held during the utterance.")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
