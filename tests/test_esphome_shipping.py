#!/usr/bin/env python3
"""Source shipping closure only; real native-client tests cover the protocol."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SatelliteShipping(unittest.TestCase):
    def test_only_esphome_satellite_is_a_production_adapter(self):
        makefile = (ROOT / "Makefile").read_text()
        targets = re.search(r"^ADAPTER_TARGETS\s*=\s*(.*)$", makefile, re.M)
        self.assertIsNotNone(targets)
        assert targets is not None
        names = targets.group(1).split()
        self.assertIn("$(BUILD)/libreecho-esphomed", names)
        self.assertNotIn("$(BUILD)/libreecho-wyomingd", names)
        self.assertIn("$(BUILD)/libreecho-sttd-wyoming", names)
        self.assertIn("$(BUILD)/libreecho-ttsd-wyoming", names)

    def test_satellite_install_preserves_wyoming_client_transports(self):
        makefile = (ROOT / "Makefile").read_text()
        install = makefile.split("\ninstall:", 1)[1]
        self.assertIn("libreecho-esphomed.init", install)
        self.assertNotIn("libreecho-wyomingd.init", install)
        self.assertNotIn("config/wyoming.service", install)
        for source in ("wyoming_client.c", "wyoming_client.h", "wyoming_protocol.c",
                       "wyoming_protocol.h", "stt_engine_wyoming.c",
                       "tts_engine_wyoming.c"):
            self.assertTrue((ROOT / "src/adapter" / source).is_file(), source)

    def test_new_protocol_tests_are_executed_by_the_repository_runner(self):
        runner = (ROOT / "tests/run_tests.sh").read_text()
        self.assertIn("test-esphome", runner)
        self.assertIn("test_esphome_shipping.py", runner)
        self.assertIn("test_wyoming_engines.py", runner)

    def test_lifecycle_health_media_and_wake_review_regressions_are_normal_gates(self):
        native = (ROOT / "tests/test_esphomed_run.py").read_text()
        for test in ("test_esphomed_timer_lifecycle.c", "test_esphomed_native_lifecycle.py",
                     "test_esphomed_final_spec.py", "src/adapter/timerd.c",
                     "test_esphome_health.py", "test_esphomed_media_state.py"):
            self.assertIn(test, native, test + " is absent from the normal native gate")
        makefile = (ROOT / "Makefile").read_text()
        gate = makefile.split("\ntest-esphome:", 1)[1].split("\n# Execute real HA", 1)[0]
        self.assertIn("tests/test_esphome_wake_reload.py", gate)
        self.assertIn("ESPHOMED_TEST_TLS_PREFIX=", gate)

    def test_watchdog_and_wake_daemons_link_their_json_dependencies(self):
        makefile = (ROOT / "Makefile").read_text()
        watchdog = re.search(r"^WATCHDOGD_SOURCES\s*=\s*(.*)$", makefile, re.M)
        self.assertIsNotNone(watchdog)
        assert watchdog is not None
        self.assertIn("src/json.c", watchdog.group(1))
        for target in ("libreecho-waked", "libreecho-waked-arm32"):
            rule = makefile.split("\n$(BUILD)/" + target + ":", 1)[1].split("\n\n", 1)[0]
            self.assertIn("src/json.c", rule)
            self.assertIn("src/adapter/adapter_server.c", rule)
        self.assertIn("$(BUILD)/json.wake.arm.o: src/json.c", makefile)
        objects = makefile.split("WAKE_DAEMON_ARM_OBJECTS =", 1)[1].split("\n\n", 1)[0]
        self.assertIn("$(BUILD)/json.wake.arm.o", objects)


if __name__ == "__main__":
    unittest.main()
