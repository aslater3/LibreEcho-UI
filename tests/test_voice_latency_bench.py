#!/usr/bin/env python3
"""Deterministic regression tests for the voice latency benchmark."""

import importlib.util
import json
import os
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODULE_PATH = os.path.join(ROOT, "tools", "voice_latency_bench.py")
SPEC = importlib.util.spec_from_file_location("voice_latency_bench", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load {MODULE_PATH}")
bench = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bench)


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def time(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class VoiceLatencyBenchTests(unittest.TestCase):
    def test_run_once_waits_for_first_pcm_finalization(self):
        clock = FakeClock()
        histories = iter([
            {"at_ms": 200, "first_pcm_ms": 0},
            {"at_ms": 200, "first_pcm_ms": 1776},
        ])
        args = SimpleNamespace(
            acoustic=False,
            url="http://198.51.100.10:8080",
            wake_settle=0.0,
            turn_timeout=3.0,
            speak_cmd="test-speaker",
            wake_word="Alexa",
        )

        def fake_api(url, path, *unused, **kwargs):
            if path == "wake-word/test":
                return {"data": {}}
            if path == "assistant/history":
                return {"data": {"turns": [next(histories)]}}
            raise AssertionError(path)

        with mock.patch.object(bench, "api", side_effect=fake_api), \
                mock.patch.object(bench, "speak"), \
                mock.patch.object(bench.time, "time", side_effect=clock.time), \
                mock.patch.object(bench.time, "sleep", side_effect=clock.sleep):
            turn = bench.run_once(args, "token", "csrf", "test phrase",
                                  {"at_ms": 100, "first_pcm_ms": 40})

        self.assertEqual(turn["first_pcm_ms"], 1776)
        self.assertGreaterEqual(clock.now, 0.5)

    def test_main_refreshes_baseline_after_a_timeout(self):
        clock = FakeClock()
        histories = iter([
            {"at_ms": 100, "first_pcm_ms": 50, "stt_total_ms": 100,
             "stt_audio_ms": 50, "stt_processing_ms": 50, "first_text_ms": 10},
            {"at_ms": 100, "first_pcm_ms": 50, "stt_total_ms": 100,
             "stt_audio_ms": 50, "stt_processing_ms": 50, "first_text_ms": 10},
            {"at_ms": 200, "first_pcm_ms": 90, "stt_total_ms": 900,
             "stt_audio_ms": 400, "stt_processing_ms": 500, "first_text_ms": 80},
            {"at_ms": 300, "first_pcm_ms": 1800, "stt_total_ms": 1800,
             "stt_audio_ms": 800, "stt_processing_ms": 1000, "first_text_ms": 100},
        ])
        calls = []
        args = [
            "voice_latency_bench.py", "--url", "http://198.51.100.10:8080",
            "-n", "2", "--turn-timeout", "0.5", "--wake-settle", "0",
            "--gap", "0", "--report", os.path.join(tempfile.gettempdir(),
                                                    "voice-latency-regression.json"),
        ]

        def fake_api(url, path, *unused, **kwargs):
            calls.append(path)
            if path == "voice-pipeline":
                return {"data": {"listening": {}}}
            if path == "assistant/history":
                return {"data": {"turns": [next(histories)]}}
            if path == "wake-word/test":
                return {"data": {}}
            if path == "audio":
                return {"data": {"amplifier_on": False}}
            raise AssertionError(path)

        report_path = args[-1]
        try:
            with mock.patch.object(sys, "argv", args), \
                    mock.patch.dict(os.environ, {
                        "LIBREECHO_USERNAME": "tester",
                        "LIBREECHO_PASSWORD": "not-a-real-password",
                    }, clear=False), \
                    mock.patch.object(bench, "login", return_value=("token", "csrf")), \
                    mock.patch.object(bench, "api", side_effect=fake_api), \
                    mock.patch.object(bench, "speak"), \
                    mock.patch.object(bench.time, "time", side_effect=clock.time), \
                    mock.patch.object(bench.time, "sleep", side_effect=clock.sleep):
                result = bench.main()
            with open(report_path, encoding="utf-8") as handle:
                report = json.load(handle)
        finally:
            try:
                os.unlink(report_path)
            except FileNotFoundError:
                pass

        # The first turn timed out. Its late record was consumed as the second
        # iteration's baseline, not misattributed to the second phrase.
        self.assertEqual(result, 1)
        self.assertEqual(report["failures"], 1)
        self.assertEqual(len(report["turns"]), 1)
        self.assertEqual(report["turns"][0]["phrase"], "what is the weather")
        self.assertEqual(report["turns"][0]["first_pcm_ms"], 1800)
        history_positions = [i for i, path in enumerate(calls)
                             if path == "assistant/history"]
        self.assertGreaterEqual(len(history_positions), 4)
        self.assertLess(history_positions[2], history_positions[3])


if __name__ == "__main__":
    unittest.main()
