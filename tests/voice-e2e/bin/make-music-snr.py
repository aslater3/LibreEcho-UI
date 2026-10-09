#!/usr/bin/env python3
"""Barge-in fixtures: the wake utterance under a synthetic music bed.

Question under test: can "Alexa" still fire while the speaker is playing
internet radio?  The device has no working AEC on the wake stream (nothing
writes the reference socket waked binds), so what reaches the classifier is
speech plus whatever the speaker is putting into the room.  These fixtures
model exactly that: the same alexa-what-time utterance, summed with a music
bed at a controlled speech-to-music ratio.

The bed is synthesised, not downloaded, so the fixtures are reproducible from
source: a four-chord progression of harmonic-rich tones, a kick and hat
pattern, and a band-limited noise floor.  That spreads energy across the whole
speech band, which is what actually masks a wake word -- a pure tone would not.

SNR is speech RMS over the utterance window divided by bed RMS over the same
window, in dB.  Positive = speech louder.  A lead of bed-only audio is
prepended so the classifier's 1.5 s context window is already full of music
before the wake word arrives; without it the test flatters the detector.

If the sum would exceed int16, the WHOLE mix (speech and bed together) is
scaled down by one common factor.  That preserves the SNR under test and is
reported in the metadata.
"""
import argparse, array, json, math, wave

RATE = 16000

# ---------------------------------------------------------------- music bed
def _rng():
    s = 0x5eed1234
    while True:
        s = (s * 1103515245 + 12345) & 0x7fffffff
        yield s / 0x3fffffff - 1.0          # uniform -1..1, deterministic

def music_bed(n):
    """Chords + beat + band-limited noise, float, roughly unit RMS."""
    r = _rng()
    # A minor -> F -> C -> G, one bar each, as MIDI-ish frequencies
    prog = [(220.00, 261.63, 329.63),      # Am
            (174.61, 220.00, 261.63),      # F
            (261.63, 329.63, 392.00),      # C
            (196.00, 246.94, 293.66)]      # G
    bar = int(RATE * 2.0)
    out = [0.0] * n

    # sustained, harmonic-rich chord tones with a slow vibrato
    for i in range(n):
        chord = prog[(i // bar) % len(prog)]
        t = i / RATE
        env = 0.0
        pos = (i % bar) / bar
        # gentle swell so it is not a static drone
        amp = 0.55 + 0.45 * math.sin(2 * math.pi * pos - math.pi / 2) ** 2
        for f in chord:
            vib = 1.0 + 0.003 * math.sin(2 * math.pi * 5.0 * t)
            for h, hw in ((1, 1.0), (2, 0.45), (3, 0.22), (4, 0.12), (6, 0.06)):
                env += hw * math.sin(2 * math.pi * f * h * vib * t)
        out[i] = 0.10 * amp * env

    # beat: kick every 0.5 s, hat every 0.25 s
    kick = int(RATE * 0.5)
    hat = int(RATE * 0.25)
    for i in range(n):
        k = i % kick
        if k < int(RATE * 0.12):
            tt = k / RATE
            out[i] += 0.9 * math.exp(-28.0 * tt) * \
                math.sin(2 * math.pi * (95.0 - 45.0 * tt / 0.12) * tt)
        h = i % hat
        if h < int(RATE * 0.05):
            tt = h / RATE
            out[i] += 0.35 * math.exp(-70.0 * tt) * next(r)

    # band-limited noise floor: one-pole high-passed white, i.e. hiss with
    # the rumble taken out, sitting under everything in the consonant band
    prev_in = prev_out = 0.0
    for i in range(n):
        x = next(r)
        y = 0.85 * (prev_out + x - prev_in)
        prev_in, prev_out = x, y
        out[i] += 0.22 * y

    rms = math.sqrt(sum(v * v for v in out) / n) or 1.0
    return [v / rms for v in out]

# ------------------------------------------------------------------ helpers
def read_wav(path):
    w = wave.open(path, 'rb')
    assert w.getnchannels() == 1 and w.getsampwidth() == 2 and \
        w.getframerate() == RATE, "speech fixture must be 16 kHz mono S16"
    d = array.array('h')
    d.frombytes(w.readframes(w.getnframes()))
    w.close()
    return d

def rms_of(seq):
    return math.sqrt(sum(float(v) * v for v in seq) / len(seq)) if seq else 0.0

def speech_window(pcm, floor_ratio=0.10):
    """First..last 10 ms block above 10% of the loudest block: the utterance."""
    blk = 160
    lv = [rms_of(pcm[i:i + blk]) for i in range(0, len(pcm) - blk, blk)]
    thr = max(lv) * floor_ratio
    on = [i for i, v in enumerate(lv) if v >= thr]
    return on[0] * blk, (on[-1] + 1) * blk

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--speech", required=True)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--meta", required=True)
    ap.add_argument("--prefix", default="music")
    ap.add_argument("--lead", type=float, default=2.0,
                    help="seconds of bed-only audio before the utterance")
    ap.add_argument("--snr", type=float, nargs="*",
                    default=[20.0, 12.0, 6.0, 0.0, -6.0])
    args = ap.parse_args()

    speech = read_wav(args.speech)
    lead = int(RATE * args.lead)
    total = lead + len(speech)
    s0, s1 = speech_window(speech)
    s_rms = rms_of(speech[s0:s1])

    bed = music_bed(total)
    bed_win = bed[lead + s0:lead + s1]
    bed_rms = rms_of(bed_win)                 # ~1.0 by construction, measured

    records = []
    # clean control: the speech alone, with the same silent lead, so the
    # comparison is fixture-shape-for-fixture-shape and not "different file"
    cases = [("clean", None)] + [("%+03d" % s, s) for s in args.snr]
    # bed alone, no speech: does music on its own trip the detector?
    cases.append(("bedonly", "bed"))

    for tag, snr in cases:
        mix = [0.0] * total
        if snr == "bed":
            g = s_rms / bed_rms                # bed at the speech's own level
            for i in range(total):
                mix[i] = g * bed[i]
        else:
            for i in range(len(speech)):
                mix[lead + i] = float(speech[i])
            if snr is not None:
                g = (s_rms / (10.0 ** (snr / 20.0))) / bed_rms
                for i in range(total):
                    mix[i] += g * bed[i]

        peak = max(abs(v) for v in mix) or 1.0
        scale = 1.0 if peak <= 32767.0 else 32767.0 / peak
        pcm = array.array('h', (int(round(v * scale)) for v in mix))

        name = "%s-%s.wav" % (args.prefix, tag)
        path = "%s/%s" % (args.outdir, name)
        w = wave.open(path, 'wb')
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(pcm.tobytes()); w.close()

        # Measure the SNR from the COMPONENTS as written, not by estimating
        # the bed from the lead: the bed swells and beats, so its level in the
        # lead is not its level under the utterance.  Both components get the
        # same `scale`, so the ratio is exact.
        if snr in (None, "bed"):
            meas = None
            got_sp = rms_of(pcm[lead + s0:lead + s1])
            got_bed = 0.0 if snr is None else got_sp
        else:
            got_sp = s_rms * scale
            got_bed = rms_of([v * g * scale for v in bed_win])
            meas = 20 * math.log10(got_sp / got_bed)
        records.append(dict(fixture=name, tag=tag, requested_snr_db=snr,
                            mix_scale=round(scale, 4),
                            speech_rms_in_window=round(got_sp, 1),
                            bed_rms_in_window=round(got_bed, 1),
                            measured_snr_db=(round(meas, 2) if meas is not None
                                             else None),
                            peak=max(max(pcm), -min(pcm)),
                            seconds=round(len(pcm) / RATE, 3)))
        print("%-18s scale=%.3f speech_rms=%7.1f bed_rms=%7.1f "
              "measured_snr=%-9s peak=%5d  %.2fs" %
              (name, scale, got_sp, got_bed,
               ("%+.2f dB" % meas) if meas is not None else "n/a",
               max(max(pcm), -min(pcm)), len(pcm) / RATE))

    json.dump(dict(rate=RATE, lead_seconds=args.lead,
                   source=args.speech,
                   speech_window_ms=[s0 * 1000 // RATE, s1 * 1000 // RATE],
                   source_speech_rms=round(s_rms, 1),
                   fixtures=records),
              open(args.meta, 'w'), indent=1)

if __name__ == "__main__":
    main()
