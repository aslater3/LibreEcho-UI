"""Real ffmpeg pipe WAV, unchanged audio bytes, through the focused C fixture.

Run with --http-fixture <compiled tests/test_esphomed_http.c>, or --tls-prefix
<mbedTLS 3.6.4 prefix> to build just that fixture. Requires ffmpeg and TMPDIR.
No HA, hardware, global environment changes, or audio-header rewriting.
"""
import argparse
import os
import pathlib
import shutil
import struct
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(argv, **kwargs):
    print("+", " ".join(map(str, argv)), flush=True)
    return subprocess.run(list(map(str, argv)), cwd=ROOT, check=True, timeout=30, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--http-fixture", type=pathlib.Path)
    group.add_argument("--tls-prefix", type=pathlib.Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    scratch = os.environ.get("TMPDIR")
    if not scratch or not pathlib.Path(scratch).is_dir():
        parser.error("TMPDIR must name an existing private scratch directory")
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        parser.error("ffmpeg is required; this compatibility gate must not skip")
    with tempfile.TemporaryDirectory(prefix="esphome-ffmpeg-wav-", dir=scratch) as directory:
        d = pathlib.Path(directory)
        env = dict(os.environ, TMPDIR=str(d), PYTHONDONTWRITEBYTECODE="1")
        fixture = args.http_fixture.resolve() if args.http_fixture else d / "http"
        if args.tls_prefix:
            prefix = args.tls_prefix.resolve()
            flags = [os.environ.get("CC", "cc"), "-std=c99", "-D_POSIX_C_SOURCE=200809L",
                     "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
                     "-I" + str(prefix / "include")]
            if args.sanitize:
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            libs = [prefix / "lib" / ("lib" + name + ".a")
                    for name in ("mbedtls", "mbedx509", "mbedcrypto")]
            run(flags + ["tests/test_esphomed_http.c", "src/adapter/radio_resample.c"]
                + libs + ["-lm", "-o", fixture], env=env)
        # A bounded finite PCM input, but a nonseekable output like HA's ffmpeg.
        # Preserve ffmpeg stdout verbatim, including RIFF/data sentinel sizes.
        result = run([ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error",
                      "-f", "s16le", "-ar", "24000", "-ac", "1", "-i", "pipe:0",
                      "-c:a", "pcm_s16le", "-f", "wav", "pipe:1"],
                     input=struct.pack("<h", 1000) * 2400, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE, env=env)
        body = result.stdout
        assert 44 <= len(body) <= 2 * 1024 * 1024
        assert body[:4] == b"RIFF" and body[8:12] == b"WAVE"
        assert struct.unpack_from("<I", body, 4)[0] == 0xFFFFFFFF
        z = 12
        while z + 8 <= len(body):
            length = struct.unpack_from("<I", body, z + 4)[0]
            if body[z:z + 4] == b"data":
                assert length == 0xFFFFFFFF
                assert body[z + 8:] == struct.pack("<h", 1000) * 2400
                break
            assert length <= len(body) - z - 8
            z += 8 + length + (length & 1)
        else:
            raise AssertionError("actual ffmpeg output has no data chunk")
        capture = d / "ffmpeg-pipe.wav"
        capture.write_bytes(body)
        print(f"Captured actual nonseekable ffmpeg WAV: {len(body)} bytes; "
              "RIFF/data sizes=0xffffffff; no header changes", flush=True)
        run([fixture, capture], env=env)
    print("Real ffmpeg WAV and focused playback/HTTP regression gate: PASS", flush=True)


if __name__ == "__main__":
    main()
