#!/usr/bin/env python3
"""Measure the capture path's magnitude response from a stepped-tone recording.

Compares micd's processed mono stream against the same analysis run on the
source, so what comes out is the DSP chain's response -- unpack scaling,
calibration, beamforming, the 80 Hz high-pass and the digital gain -- rather
than the response of the test signal.

Per-tone RMS rather than a transform. A Goertzel over the whole buffer looks
tempting, but its bin index is an integer: source and capture are different
lengths, so each rounds to a slightly different analysis frequency and the two
decohere over several seconds of integration. That shows up as deep notches at
arbitrary frequencies -- a measurement artifact indistinguishable from a real
one until you check. Segmenting on the silence between tones and taking the RMS
of each segment is exact, needs no alignment, and cannot drift.
"""
import argparse, array, json, math, struct, sys, wave


def segments(samples, rate, count, floor_ratio=0.15):
    """Split a stepped-tone recording into its tones.

    Gates on a fraction of the loudest 10 ms block rather than an absolute
    level, so it works whatever gain the chain applies.
    """
    block = int(rate * 0.01)
    energies = []
    for i in range(0, len(samples) - block, block):
        w = samples[i:i + block]
        energies.append(math.sqrt(sum(float(v) * v for v in w) / len(w)))
    if not energies:
        return []
    gate = max(energies) * floor_ratio
    runs, start = [], None
    for i, e in enumerate(energies):
        if e >= gate and start is None:
            start = i
        elif e < gate and start is not None:
            if i - start >= 10:                 # ignore blips under 100 ms
                runs.append((start * block, i * block))
            start = None
    if start is not None and len(energies) - start >= 10:
        runs.append((start * block, len(energies) * block))
    return runs


def rms(samples):
    if not samples:
        return 0.0
    return math.sqrt(sum(float(v) * v for v in samples) / len(samples))


def read_wav_mono(path):
    w = wave.open(path, 'rb')
    if w.getnchannels() != 1 or w.getsampwidth() != 2:
        raise SystemExit("expected 16-bit mono: %s" % path)
    raw = w.readframes(w.getnframes()); w.close()
    a = array.array('h'); a.frombytes(raw)
    return list(a)


def read_raw_s16(path):
    with open(path, 'rb') as fh:
        raw = fh.read()
    n = len(raw) // 2
    return list(struct.unpack("<%dh" % n, raw[:n * 2]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True, help="the fixture WAV that was played")
    ap.add_argument("--captured", required=True, help="micd's processed S16 stream")
    ap.add_argument("--meta", required=True)
    ap.add_argument("--tolerance-db", type=float, default=6.0)
    ap.add_argument("--json")
    args = ap.parse_args()

    meta = json.load(open(args.meta))
    rate = meta["rate"]
    src = read_wav_mono(args.source)
    cap = read_raw_s16(args.captured)
    if len(cap) < rate:                       # under a second is not a measurement
        raise SystemExit("captured only %d samples" % len(cap))

    src_runs = segments(src, rate, len(meta["freqs"]))
    cap_runs = segments(cap, rate, len(meta["freqs"]))
    if len(src_runs) != len(meta["freqs"]):
        raise SystemExit("found %d tones in the source, expected %d"
                         % (len(src_runs), len(meta["freqs"])))
    if len(cap_runs) != len(meta["freqs"]):
        raise SystemExit("found %d tones in the capture, expected %d -- "
                         "the recording may be truncated"
                         % (len(cap_runs), len(meta["freqs"])))

    rows = []
    for f, (s0, s1), (c0, c1) in zip(meta["freqs"], src_runs, cap_runs):
        # trim the ramped edges out of both windows
        pad = int(rate * 0.02)
        sr_ = rms(src[s0 + pad:s1 - pad])
        cr_ = rms(cap[c0 + pad:c1 - pad])
        rows.append((f, 20.0 * math.log10(cr_ / sr_) if sr_ > 0 and cr_ > 0 else None))

    band = [db for f, db in rows if db is not None and 200.0 <= f <= 4000.0]
    ref = sorted(band)[len(band) // 2] if band else 0.0

    print("  capture path magnitude response (relative to 200-4000 Hz median)")
    worst = 0.0
    out = []
    for f, db in rows:
        if db is None:
            print("    %7.1f Hz   (no energy)" % f)
            continue
        rel = db - ref
        bar = "#" * max(0, int(round(20 + rel)))
        flag = ""
        if 200.0 <= f <= 4000.0 and abs(rel) > args.tolerance_db:
            flag = "  <-- outside +/-%.0f dB" % args.tolerance_db
            worst = max(worst, abs(rel))
        print("    %7.1f Hz  %+6.1f dB  %s%s" % (f, rel, bar, flag))
        out.append({"hz": f, "rel_db": round(rel, 2)})

    if args.json:
        json.dump({"reference_db": round(ref, 2), "points": out},
                  open(args.json, "w"), indent=2)
    if worst:
        print("  FAIL: passband deviates by %.1f dB" % worst)
        return 1
    print("  PASS: 200-4000 Hz within +/-%.0f dB" % args.tolerance_db)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
