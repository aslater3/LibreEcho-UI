#!/usr/bin/env python3
"""Write a stepped-tone fixture for measuring the capture path's response.

A continuous sweep would need the captured stream aligned against the source
before it could be analysed. Stepped tones do not: each tone sits at a distinct
frequency, so a Goertzel over the whole capture recovers its energy no matter
where the recording started or how much room tone padded it.

The capture path runs at 16 kHz, so the measurable band stops at Nyquist. The
20 Hz-20 kHz in issue #37 describes the *output* path; nothing above 8 kHz
exists to measure on the way in.
"""
import argparse, array, json, math, wave

RATE = 16000

def tone(freq, seconds, amplitude):
    n = int(RATE * seconds)
    out = array.array('h')
    for i in range(n):
        # fade the edges so a step does not splatter energy across the band
        env = min(1.0, i / (RATE * 0.01), (n - i) / (RATE * 0.01))
        out.append(int(amplitude * env * math.sin(2 * math.pi * freq * i / RATE)))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--meta", required=True)
    ap.add_argument("--low", type=float, default=40.0)
    ap.add_argument("--high", type=float, default=7000.0)
    ap.add_argument("--steps", type=int, default=16)
    ap.add_argument("--seconds", type=float, default=0.35)
    ap.add_argument("--amplitude", type=int, default=6000)
    args = ap.parse_args()

    freqs = [args.low * (args.high / args.low) ** (i / (args.steps - 1))
             for i in range(args.steps)]
    pcm = array.array('h')
    pcm.extend(array.array('h', [0]) * int(RATE * 0.3))      # settle
    for f in freqs:
        pcm.extend(tone(f, args.seconds, args.amplitude))
        pcm.extend(array.array('h', [0]) * int(RATE * 0.05))

    w = wave.open(args.out, 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
    w.writeframes(pcm.tobytes()); w.close()
    with open(args.meta, 'w') as fh:
        json.dump({"rate": RATE, "amplitude": args.amplitude,
                   "seconds_per_tone": args.seconds,
                   "freqs": [round(f, 1) for f in freqs]}, fh)
    print("wrote %s: %d tones %.0f-%.0f Hz, %.1fs"
          % (args.out, len(freqs), freqs[0], freqs[-1], len(pcm) / RATE))

if __name__ == "__main__":
    raise SystemExit(main())
