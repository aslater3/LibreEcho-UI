#!/usr/bin/env python3
"""Stage 1 probe: read micd's calibrated mono stream and report its level.

micd hands out one `stream_mono` client at a time, so this runs before waked
is started.  It asserts the negotiated format matches what the rest of the
pipeline assumes (S16_LE / 16 kHz / mono / calibration applied) and measures
the RMS, which is the number that goes wrong when the 24-bit unpack scaling
regresses -- the exact bug this pipeline already had.
"""
import argparse, json, math, socket, struct, sys, time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--socket", required=True)
    ap.add_argument("--seconds", type=float, default=1.5)
    ap.add_argument("--out")
    args = ap.parse_args()

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(args.socket)
    s.sendall(b'{"v":1,"id":1,"cmd":"stream_mono","args":{}}\n')
    line = b""
    while not line.endswith(b"\n"):
        c = s.recv(1)
        if not c:
            print("micd closed the connection before responding")
            return 2
        line += c
    header = json.loads(line.decode())
    if not header.get("ok"):
        print("micd refused stream_mono: %s" % header.get("error"))
        return 2
    d = header.get("data") or {}
    want = {"format": "pcm_s16_le", "rate": 16000, "channels": 1,
            "calibration_applied": True}
    for k, v in want.items():
        if d.get(k) != v:
            print("micd stream contract changed: %s=%r (expected %r)"
                  % (k, d.get(k), v))
            return 2

    deadline = time.time() + args.seconds
    raw = bytearray()
    s.settimeout(2.0)
    while time.time() < deadline:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        raw += chunk
    s.close()
    n = len(raw) // 2
    if n == 0:
        print("micd produced no audio")
        return 2
    values = struct.unpack("<%dh" % n, bytes(raw[:n * 2]))
    rms = math.sqrt(sum(float(v) * v for v in values) / n)
    peak = max(abs(v) for v in values)
    clipped = sum(1 for v in values if abs(v) >= 32767)
    if args.out:
        with open(args.out, "wb") as fh:
            fh.write(bytes(raw[:n * 2]))
    print(json.dumps({"samples": n, "ms": round(n / 16.0, 1),
                      "rms": round(rms, 1), "peak": peak,
                      "clipped": clipped,
                      "beamforming": d.get("beamforming"),
                      "selected": d.get("selected_logical_mics")}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
