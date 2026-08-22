#!/usr/bin/env python3
"""Build the wake-word/command pause matrix from real recorded speech.

Splices the two halves of the standard fixture back together with a controlled
gap rather than synthesising them, so the words themselves are identical across
the whole matrix and the gap is the only variable. Anything that changes across
these fixtures is caused by the pause and nothing else.

The boundary comes from the recording itself: "Alexa" and the command are
separated by a natural 250 ms pause, found by energy segmentation.
"""
import argparse, array, json, math, os, wave

def load(path):
    w = wave.open(path, 'rb')
    if w.getnchannels() != 1 or w.getsampwidth() != 2:
        raise SystemExit("expected 16-bit mono: %s" % path)
    a = array.array('h'); a.frombytes(w.readframes(w.getnframes()))
    sr = w.getframerate(); w.close()
    return a, sr

def boundary(samples, rate):
    """Locate the widest internal silence -- the wake word/command split."""
    blk = int(rate * 0.01)
    e = [math.sqrt(sum(float(v) * v for v in samples[i:i + blk]) / blk)
         for i in range(0, len(samples) - blk, blk)]
    gate = max(e) * 0.08
    voiced = [i for i, v in enumerate(e) if v >= gate]
    if not voiced:
        raise SystemExit("no speech found")
    best, prev = None, voiced[0]
    for i in voiced[1:]:
        if i - prev > 4 and (best is None or (i - prev) > (best[1] - best[0])):
            best = (prev, i)
        prev = i
    if not best:
        raise SystemExit("no internal pause found")
    return voiced[0] * blk, best[0] * blk, best[1] * blk, voiced[-1] * blk

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--gaps", default="0,250,500,1000,1500,2000,3000")
    ap.add_argument("--lead-ms", type=int, default=700)
    ap.add_argument("--tail-ms", type=int, default=1500)
    args = ap.parse_args()

    a, sr = load(args.source)
    s0, w_end, c_start, s1 = boundary(a, sr)
    wake = a[s0:w_end]
    command = a[c_start:min(s1 + int(sr * 0.05), len(a))]
    os.makedirs(args.outdir, exist_ok=True)
    silence = lambda ms: array.array('h', [0]) * int(sr * ms / 1000.0)

    made = []
    for g in [int(x) for x in args.gaps.split(",")]:
        pcm = silence(args.lead_ms) + wake + silence(g) + command + silence(args.tail_ms)
        name = "pause-%04d.wav" % g
        w = wave.open(os.path.join(args.outdir, name), 'wb')
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
        w.writeframes(pcm.tobytes()); w.close()
        made.append({"gap_ms": g, "file": name,
                     "wake_ms": int(len(wake) * 1000 / sr),
                     "command_ms": int(len(command) * 1000 / sr),
                     "command_starts_ms": int(args.lead_ms + len(wake) * 1000 / sr + g)})
    json.dump({"rate": sr, "fixtures": made},
              open(os.path.join(args.outdir, "pause-matrix.json"), "w"), indent=2)
    print("wake word %d ms, command %d ms, natural gap %d ms"
          % (len(wake) * 1000 / sr, len(command) * 1000 / sr,
             (c_start - w_end) * 1000 / sr))
    for m in made:
        print("  %-16s gap %4d ms  command starts at %d ms"
              % (m["file"], m["gap_ms"], m["command_starts_ms"]))

if __name__ == "__main__":
    raise SystemExit(main())
