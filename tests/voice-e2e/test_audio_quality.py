#!/usr/bin/env python3
"""Host-only math and artifact-contract tests for issue #37 audio analysis."""
import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import wave

ROOT = pathlib.Path(__file__).resolve().parent
LIB = ROOT / "lib"
sys.path.insert(0, str(LIB))

from capture_quality import dbfs, goertzel_rms, rms  # noqa: E402
from capture_response import estimate_impulse_latency  # noqa: E402


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def test_tone_math():
    rate = 16000
    samples = [12000 if i % 16 == 0 else 0 for i in range(rate)]
    check(abs(rms(samples) - 3000.0) < 1e-9, "RMS must be deterministic")
    check(abs(dbfs(32768.0)) < 1e-9, "full scale must be 0 dBFS")
    tone = [int(12000 * __import__("math").sin(2 * __import__("math").pi * 1000 * i / rate)) for i in range(rate)]
    check(abs(goertzel_rms(tone, 1000, rate) - 12000 / 2 ** 0.5) < 1.0,
          "Goertzel RMS must recover an exact-bin sine")


def test_impulse_latency_math():
    source = [0] * 32 + [1000] + [0] * 32
    lead = 1500
    observed = [0] * (lead + 7) + [v * 3 for v in source] + [0] * 8
    result = estimate_impulse_latency(
        source, observed, 16000, capture_lead_samples=lead,
    )
    check(result["latency_samples"] == 7, "impulse latency must exclude fixture lead")
    check(abs(result["latency_ms"] - 0.4375) < 1e-9, "latency conversion must be exact")
    check(len(result["response_samples"]) <= 65, "impulse artifact must stay bounded")


def test_sweep_is_deterministic_and_labeled():
    generator = ROOT / "bin" / "make-sweep.py"
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        wav_a, meta_a = tmp / "a.wav", tmp / "a.json"
        wav_b, meta_b = tmp / "b.wav", tmp / "b.json"
        for wav, meta in ((wav_a, meta_a), (wav_b, meta_b)):
            subprocess.run([sys.executable, str(generator), "--out", str(wav), "--meta", str(meta)],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        check(hashlib.sha256(wav_a.read_bytes()).digest() == hashlib.sha256(wav_b.read_bytes()).digest(),
              "sweep WAV must be byte deterministic")
        check(meta_a.read_bytes() == meta_b.read_bytes(), "sweep metadata must be deterministic")
        meta = json.loads(meta_a.read_text())
        check(meta["evidence_class"] == "software_capture_chain", "fixture must label software evidence")
        check(meta["acoustic_path_measured"] is False, "fixture must not claim acoustic evidence")
        check(meta["capture_lead_ms"] == 1500, "capture lead must be explicit")
        check(meta["impulse_index"] > 0, "fixture must contain an impulse marker")
        check(meta["artifact_limits"]["max_capture_bytes"] <= 8 * 1024 * 1024,
              "capture artifact bound must be explicit")


def test_quality_report_contract():
    generator = ROOT / "bin" / "make-levels.py"
    analyzer = LIB / "capture_quality.py"
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        fixture = tmp / "levels.wav"
        meta = tmp / "levels.json"
        captured = tmp / "captured.raw"
        report_path = tmp / "report.json"
        subprocess.run(
            [sys.executable, str(generator), "--out", str(fixture), "--meta", str(meta)],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        with wave.open(str(fixture), "rb") as source:
            captured.write_bytes(source.readframes(source.getnframes()))
        subprocess.run(
            [sys.executable, str(analyzer), "--captured", str(captured),
             "--meta", str(meta), "--json", str(report_path)],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        report = json.loads(report_path.read_text())
    check(report["evidence_class"] == "software_capture_chain", "report evidence class drift")
    check(report["acoustic_path_measured"] is False, "report must not claim acoustic evidence")
    check(report["hardware_acceptance"] == "not_measured", "report hardware label drift")
    check(report["artifact_limits"]["max_levels"] == len(report["levels"]),
          "report level bound must describe emitted levels")
    check(len(report["levels"]) == 7, "report must contain all synthetic quality levels")


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn()
            print("PASS %s" % name)
    print("audio quality math: all pass")
