#!/usr/bin/env python3
"""Fixture for noise floor, THD+N and clipping headroom (issue #37 part 3).

Silence first, then one frequency at rising amplitudes. A single frequency
keeps the analysis unambiguous: everything that is not the fundamental is
distortion or noise, so THD+N falls out of one buffer without needing the
source for reference.
"""
import argparse, array, json, math, wave

RATE = 16000

def tone(freq, seconds, amplitude):
    n = int(RATE * seconds)
    out = array.array('h')
    for i in range(n):
        env = min(1.0, i / (RATE * 0.01), (n - i) / (RATE * 0.01))
        v = int(amplitude * env * math.sin(2 * math.pi * freq * i / RATE))
        out.append(32767 if v > 32767 else (-32768 if v < -32768 else v))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--meta", required=True)
    ap.add_argument("--freq", type=float, default=1000.0)
    ap.add_argument("--seconds", type=float, default=0.5)
    args = ap.parse_args()

    # spans a quiet room through to a shout at the grille
    levels = [500, 1000, 2000, 4000, 8000, 16000, 30000]
    pcm = array.array('h')
    pcm.extend(array.array('h', [0]) * int(RATE * 1.0))     # noise floor window
    for a in levels:
        pcm.extend(tone(args.freq, args.seconds, a))
        pcm.extend(array.array('h', [0]) * int(RATE * 0.15))

    w = wave.open(args.out, 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
    w.writeframes(pcm.tobytes()); w.close()
    json.dump({"rate": RATE, "freq": args.freq, "levels": levels,
               "seconds_per_tone": args.seconds}, open(args.meta, 'w'))
    print("wrote %s: %d levels at %.0f Hz, %.1fs"
          % (args.out, len(levels), args.freq, len(pcm) / RATE))

if __name__ == "__main__":
    raise SystemExit(main())
