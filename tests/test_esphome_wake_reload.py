#!/usr/bin/env python3
"""Bounded host lifecycle regression; ONNX compute alone is scripted.

Normal runner: python3 tests/test_esphome_wake_reload.py
Optional staged SpeexDSP: SPEEXDSP_PREFIX=/path/to/prefix
All compiler/test outputs use the caller's explicit private TMPDIR.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    "tests/test_esphome_wake_reload.c",
    "src/adapter/adapter_server.c", "src/adapter/adapter_client.c",
    "src/adapter/wake_led.c", "src/adapter/voice_stream.c",
    "src/adapter/voice_aec.c", "src/adapter/voice_reference.c",
    "src/adapter/voice_dsp.c", "src/log.c", "src/json.c",
]
CASES = ["validation", "reload", "failure", "recovery", "queued", "capture",
         "timeout", "retirement"]


def main():
    scratch = os.environ.get("TMPDIR")
    if not scratch or not Path(scratch).is_dir():
        raise SystemExit("Set TMPDIR to a private existing scratch directory")
    prefix = os.environ.get("SPEEXDSP_PREFIX")
    speex = ([f"-I{prefix}/include", f"{prefix}/lib/libspeexdsp.a"]
             if prefix else ["-lspeexdsp"])
    failed = False
    cases = sys.argv[1:] or CASES
    if any(case not in CASES for case in cases):
        raise SystemExit("Unknown lifecycle case")
    with tempfile.TemporaryDirectory(prefix="wake-reload-", dir=scratch) as work:
        directory = Path(work)
        models = directory / "models"
        models.mkdir()
        for enabled in (True, False):
            binary = directory / ("enabled" if enabled else "disabled")
            cmd = shlex.split(os.environ.get("CC", "cc")) + [
                "-D_POSIX_C_SOURCE=200809L", "-std=c99", "-Wall", "-Wextra",
                "-Wpedantic", "-Werror", "-ffunction-sections", "-fdata-sections",
                "-Wl,--gc-sections", "-Isrc", "-Isrc/adapter",
            ]
            if enabled:
                cmd += ["-DLE_WAKE_ENGINE_ONNX"]
            cmd += SOURCES
            if enabled:
                cmd += ["src/adapter/wake_worker.c"]
            cmd += speex + ["-lpthread", "-lm", "-o", str(binary)]
            subprocess.run(cmd, cwd=ROOT, check=True, timeout=30)
            for case in cases if enabled else ["disabled"]:
                result = subprocess.run([str(binary), case, str(models)],
                                        cwd=ROOT, timeout=10,
                                        capture_output=True, text=True)
                print(result.stdout + result.stderr, end="", flush=True)
                print(f"{case}: exit={result.returncode}", flush=True)
                failed |= result.returncode != 0
    raise SystemExit(1 if failed else 0)


if __name__ == "__main__":
    main()
