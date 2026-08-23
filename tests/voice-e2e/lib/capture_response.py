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

MAX_CAPTURE_BYTES = 8 * 1024 * 1024


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
        raw = fh.read(MAX_CAPTURE_BYTES + 1)
    if len(raw) > MAX_CAPTURE_BYTES:
        raise SystemExit("capture exceeds %d-byte artifact limit" % MAX_CAPTURE_BYTES)
    n = len(raw) // 2
    return list(struct.unpack("<%dh" % n, raw[:n * 2]))


def estimate_impulse_latency(source, captured, rate, source_index=None,
                             response_window=65, capture_lead_samples=0):
    """Find the marked impulse in the captured software stream.

    The marker makes this a bounded stream-alignment measurement, not an
    acoustic speaker/microphone latency claim.  A coarse onset locates the
    capture window, then the peak is selected only around the known marker.
    """
    if not source or not captured:
        raise ValueError("source and capture must contain samples")
    if source_index is None:
        source_index = max(range(len(source)), key=lambda i: abs(source[i]))
    if source_index < 0 or source_index >= len(source):
        raise ValueError("source impulse index is outside the source")
    source_peak = abs(source[source_index])
    if source_peak == 0:
        raise ValueError("source impulse must be non-zero")
    threshold = max(1, source_peak // 4)
    source_onset = next((i for i, v in enumerate(source)
                         if abs(v) >= threshold), source_index)
    capture_threshold = max(1, threshold)
    capture_onset = next((i for i, v in enumerate(captured)
                          if abs(v) >= capture_threshold), None)
    if capture_onset is None:
        raise ValueError("capture does not contain the impulse marker")
    coarse = capture_onset - source_onset
    radius = max(1, rate // 20)
    expected = source_index + coarse
    lo = max(0, expected - radius)
    hi = min(len(captured), expected + radius + 1)
    observed_index = max(range(lo, hi), key=lambda i: abs(captured[i]))
    half = response_window // 2
    response = captured[max(0, observed_index - half):
                        min(len(captured), observed_index + half + 1)]
    latency = observed_index - source_index - capture_lead_samples
    return {"latency_samples": latency,
            "latency_ms": latency * 1000.0 / rate,
            "source_impulse_index": source_index,
            "captured_impulse_index": observed_index,
            "response_samples": response}


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

    capture_lead_samples = int(round(
        float(meta.get("capture_lead_ms", 0)) * rate / 1000.0
    ))
    impulse = estimate_impulse_latency(
        src, cap, rate, meta.get("impulse_index"),
        capture_lead_samples=capture_lead_samples,
    )
    print("  software stream impulse: %+d samples (%.3f ms)"
          % (impulse["latency_samples"], impulse["latency_ms"]))
    if args.json:
        json.dump({"evidence_class": "software_capture_chain",
                   "acoustic_path_measured": False,
                   "hardware_acceptance": "not_measured",
                   "measurement_band_hz": [min(meta["freqs"]),
                                           max(meta["freqs"])],
                   "reference_db": round(ref, 2), "points": out,
                   "impulse_response": impulse},
                  open(args.json, "w"), indent=2)
    if worst:
        print("  FAIL: passband deviates by %.1f dB" % worst)
        return 1
    print("  PASS: 200-4000 Hz within +/-%.0f dB" % args.tolerance_db)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
