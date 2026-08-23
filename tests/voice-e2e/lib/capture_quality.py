#!/usr/bin/env python3
"""Noise floor, THD+N and clipping headroom for the capture path (#37 part 3).

Every number here comes from a single captured buffer, so the analysis-window
mismatch that can fake a notch when comparing two recordings of different
lengths cannot arise: the fundamental and the total are measured over exactly
the same samples.
"""
import argparse, array, json, math, struct

MAX_CAPTURE_BYTES = 8 * 1024 * 1024


def goertzel_rms(samples, freq, rate):
    """RMS of one frequency component over this exact buffer."""
    n = len(samples)
    k = int(0.5 + n * freq / rate)
    w = 2.0 * math.pi * k / n
    coeff = 2.0 * math.cos(w)
    s1 = s2 = 0.0
    for x in samples:
        s0 = x + coeff * s1 - s2
        s2, s1 = s1, s0
    power = s1 * s1 + s2 * s2 - coeff * s1 * s2
    # magnitude of a real sinusoid -> RMS
    return math.sqrt(max(power, 0.0)) * math.sqrt(2.0) / n


def rms(samples):
    if not samples:
        return 0.0
    return math.sqrt(sum(float(v) * v for v in samples) / len(samples))


def dbfs(v):
    return -999.0 if v <= 0 else 20.0 * math.log10(v / 32768.0)


def segments(samples, rate, min_blocks=10):
    """Split on silence, gating relative to the noise floor.

    Gating on a fraction of the loudest block cannot work here: these levels
    span 500 to 30000, so a gate set from the peak swallows the quiet tones
    whole and the run count comes up short.
    """
    block = int(rate * 0.01)
    energies = [rms(samples[i:i + block])
                for i in range(0, len(samples) - block, block)]
    if not energies:
        return []
    quiet = sorted(energies)[:max(1, len(energies) // 10)]
    floor = sum(quiet) / len(quiet)
    gate = max(floor * 4.0, max(energies) * 0.002)
    runs, start = [], None
    for i, e in enumerate(energies):
        if e >= gate and start is None:
            start = i
        elif e < gate and start is not None:
            if i - start >= min_blocks:
                runs.append((start * block, i * block))
            start = None
    if start is not None and len(energies) - start >= min_blocks:
        runs.append((start * block, len(energies) * block))
    return runs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--captured", required=True)
    ap.add_argument("--meta", required=True)
    ap.add_argument("--json")
    ap.add_argument("--gain-tolerance-pct", type=float, default=5.0)
    ap.add_argument("--thdn-tolerance-pct", type=float, default=1.0)
    args = ap.parse_args()

    meta = json.load(open(args.meta))
    rate, freq, levels = meta["rate"], meta["freq"], meta["levels"]
    raw = open(args.captured, "rb").read(MAX_CAPTURE_BYTES + 1)
    if len(raw) > MAX_CAPTURE_BYTES:
        raise SystemExit("capture exceeds %d-byte artifact limit" % MAX_CAPTURE_BYTES)
    n = len(raw) // 2
    cap = list(struct.unpack("<%dh" % n, raw[:n * 2]))
    if n < rate:
        raise SystemExit("captured only %d samples" % n)

    runs = segments(cap, rate)
    if len(runs) != len(levels):
        raise SystemExit("found %d tones, expected %d -- capture may be "
                         "truncated" % (len(runs), len(levels)))

    # The noise floor is whatever precedes the first tone.
    quiet = cap[:max(0, runs[0][0] - int(rate * 0.05))]
    floor = rms(quiet)
    print("  noise floor: %.1f rms (%.1f dBFS) over %.2fs of silence"
          % (floor, dbfs(floor), len(quiet) / float(rate)))
    print()
    print("  in amp   in rms   out rms   gain    THD+N     clipped")
    out = []
    headroom = None
    for level, (a, b) in zip(levels, runs):
        pad = int(rate * 0.02)
        seg = cap[a + pad:b - pad]
        total = rms(seg)
        fund = goertzel_rms(seg, freq, rate)
        resid = math.sqrt(max(total * total - fund * fund, 0.0))
        thdn = (resid / fund * 100.0) if fund > 0 else float("inf")
        clipped = sum(1 for v in seg if abs(v) >= 32767)
        # `level` is the tone's amplitude; a sine's RMS is amplitude/sqrt(2).
        # Dividing an RMS by an amplitude understates the gain by 1.41x --
        # it reported 2.77x for a chain whose digital gain is 4.00x.
        source_rms = level / math.sqrt(2.0)
        gain = (total / source_rms) if source_rms else 0.0
        if clipped and headroom is None:
            headroom = level
        print("  %6d  %7.0f  %8.0f  %5.2fx  %6.2f%%   %d"
              % (level, source_rms, total, gain, thdn, clipped))
        out.append({"in_amplitude": level, "in_rms": round(source_rms, 1),
                    "out_rms": round(total, 1),
                    "gain": round(gain, 3), "thd_n_pct": round(thdn, 3),
                    "clipped": clipped})

    # Assertions, so this catches a regression rather than only describing one.
    # Only the pre-clipping levels are judged: above the ceiling the chain is
    # supposed to distort, and saying so is the measurement, not a failure.
    linear = [r for r in out if not r["clipped"]]
    failures = []
    if len(linear) < 3:
        failures.append("only %d unclipped levels; nothing to judge linearity on"
                        % len(linear))
    else:
        gains = [r["gain"] for r in linear]
        spread = (max(gains) - min(gains)) / (sum(gains) / len(gains)) * 100.0
        print()
        print("  gain linearity: %.2f%% spread over %d unclipped levels (%.2fx-%.2fx)"
              % (spread, len(linear), min(gains), max(gains)))
        if spread > args.gain_tolerance_pct:
            failures.append("gain spread %.2f%% exceeds %.2f%%"
                            % (spread, args.gain_tolerance_pct))
        worst_thdn = max(r["thd_n_pct"] for r in linear)
        print("  worst THD+N below clipping: %.2f%%" % worst_thdn)
        if worst_thdn > args.thdn_tolerance_pct:
            failures.append("THD+N %.2f%% exceeds %.2f%%"
                            % (worst_thdn, args.thdn_tolerance_pct))

    print()
    if headroom is None:
        print("  clipping headroom: no clipping up to input %d" % levels[-1])
    else:
        print("  clipping headroom: output clips from input %d "
              "(%.1f dBFS at the microphone)" % (headroom, dbfs(headroom)))
    if failures:
        print()
        for f in failures:
            print("  FAIL: %s" % f)
    if args.json:
        json.dump({"evidence_class": "software_capture_chain",
                   "acoustic_path_measured": False,
                   "hardware_acceptance": "not_measured",
                   "artifact_limits": {"max_capture_bytes": MAX_CAPTURE_BYTES,
                                       "max_levels": len(levels)},
                   "noise_floor_rms": round(floor, 2),
                   "noise_floor_dbfs": round(dbfs(floor), 2),
                   "clipping_from_input": headroom, "levels": out},
                  open(args.json, "w"), indent=2)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
