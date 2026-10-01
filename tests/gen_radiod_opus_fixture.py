#!/usr/bin/env python3
"""Regenerate tests/radiod_opus_fixture.h from fresh Ogg Opus fixtures.

The checked-in header is canonical: tests/test_radio_opus.c asserts the exact
decoded frame counts (stereo 4800, mono 4800, chained 9600).  Run this only to
refresh the bytes; if a fixture's frame count changes, update those constants
too.  It needs ffmpeg with an Opus encoder and nothing else.

    python3 tests/gen_radiod_opus_fixture.py [output-header]

The fixtures are synthetic tones, so they carry no third-party content.
"""
import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(REPO, "tests", "radiod_opus_fixture.h")

# One 0.1 s stereo and mono tone, then the fixtures derived from them.
BASE = ("sine=frequency=440:duration=0.1:sample_rate=48000")


def run(cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)


def encode(ffmpeg, path, channels):
    run([ffmpeg, "-hide_banner", "-y", "-f", "lavfi", "-i", BASE,
         "-ac", str(channels), "-c:a", "libopus", "-b:a", "64k", path])


def build(ffmpeg, outdir):
    stereo = os.path.join(outdir, "stereo.ogg")
    stereo2 = os.path.join(outdir, "stereo2.ogg")
    mono = os.path.join(outdir, "mono.ogg")
    chained = os.path.join(outdir, "chained.ogg")
    truncated = os.path.join(outdir, "truncated.ogg")
    badhead = os.path.join(outdir, "badhead.ogg")
    junk = os.path.join(outdir, "junk.ogg")

    encode(ffmpeg, stereo, 2)
    encode(ffmpeg, stereo2, 2)
    encode(ffmpeg, mono, 1)
    # A chained stream is two complete Opus streams concatenated.  They must be
    # distinct encodes: Ogg logical streams carry a serial number, and a
    # decoder rejects two links that reuse one.
    with open(stereo, "rb") as f:
        first = f.read()
    with open(stereo2, "rb") as f:
        second = f.read()
    with open(chained, "wb") as f:
        f.write(first)
        f.write(second)
    # Truncation: keep the Ogg header, cut the payload.
    with open(truncated, "wb") as f:
        f.write(first[:180])
    # Bad header: keep the Ogg capture pattern, corrupt the OpusHead magic.
    corrupt = bytearray(first[:44])
    for i, byte in enumerate(corrupt):
        if corrupt[i:i + 8] == b"OpusHead":
            corrupt[i] ^= 0xFF
            break
    with open(badhead, "wb") as f:
        f.write(bytes(corrupt))
    with open(junk, "wb") as f:
        f.write(b"not an ogg opus stream at all\n")
    return {"stereo": stereo, "mono": mono, "chained": chained,
            "truncated": truncated, "badhead": badhead, "junk": junk}


def emit(paths, out_path):
    order = ["stereo", "mono", "chained", "truncated", "badhead", "junk"]
    lines = []
    lines.append("/*")
    lines.append(" * Ogg Opus decode fixtures for tests/test_radio_opus.c.")
    lines.append(" *")
    lines.append(" * Generated, not sampled: each array is the exact byte capture of a tiny")
    lines.append(" * Ogg Opus file produced by tests/gen_radiod_opus_fixture.py (ffmpeg +")
    lines.append(" * libopus).  They carry no third-party audio; see")
    lines.append(" * THIRD_PARTY_NOTICES.md.  Regenerate with that script if the decoder")
    lines.append(" * contract ever changes.")
    lines.append(" *")
    lines.append(" *   stereo    - one stereo Opus stream, 4800 decoded frames")
    lines.append(" *   mono      - one mono Opus stream, 4800 decoded frames")
    lines.append(" *   chained   - two Opus streams chained in one Ogg, 9600 frames total")
    lines.append(" *   truncated - Ogg Opus header with the payload cut off")
    lines.append(" *   badhead   - Ogg capture pattern with a corrupt OpusHead")
    lines.append(" *   junk      - no Ogg structure at all")
    lines.append(" */")
    lines.append("#ifndef LIBREECHO_RADIOD_OPUS_FIXTURE_H")
    lines.append("#define LIBREECHO_RADIOD_OPUS_FIXTURE_H")
    lines.append("")
    for name in order:
        with open(paths[name], "rb") as f:
            data = f.read()
        items = ["0x%02x," % b for b in data]
        lines.append("static const unsigned char le_opus_%s[] = {" % name)
        for i in range(0, len(items), 12):
            lines.append("    " + " ".join(items[i:i + 12]))
        lines.append("};")
        lines.append("static const unsigned long le_opus_%s_len = %dUL;"
                     % (name, len(data)))
        lines.append("")
    lines.append("#endif /* LIBREECHO_RADIOD_OPUS_FIXTURE_H */")
    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_OUT
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        sys.stderr.write("ffmpeg is required to regenerate the fixtures\n")
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        paths = build(ffmpeg, tmp)
        emit(paths, out)
    print("wrote %s" % out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
