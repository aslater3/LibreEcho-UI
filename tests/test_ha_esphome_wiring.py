"""Network-free contract for the distinct real-HA acceptance gate, not runtime proof."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RealHAGateWiring(unittest.TestCase):
    def test_make_target_executes_actual_home_assistant_fixture(self):
        makefile = (ROOT / 'Makefile').read_text()
        match = re.search(r'^test-ha-esphome:[^\n]*\n((?:\t[^\n]*\n)+)', makefile, re.M)
        self.assertIsNotNone(match, 'real-HA gate must be separately executable')
        assert match is not None
        recipe = match.group(1)
        self.assertIn('tests/test_ha_esphome_integration.py', recipe)
        self.assertIn('ESPHOME_HA_PYTHON="$(ESPHOME_HA_PYTHON)"', recipe)
        self.assertIn('ESPHOME_TLS_PREFIX="$(ESPHOMED_TEST_TLS_PREFIX)"', recipe)
        self.assertNotIn('|| true', recipe)

    def test_normal_ci_provisions_pinned_ha_and_executes_gate(self):
        workflow = (ROOT / '.github/workflows/checks.yml').read_text()
        self.assertIn('actions/setup-python@ece7cb06caefa5fff74198d8649806c4678c61a1', workflow)
        self.assertIn("python-version: '3.14.7'", workflow)
        self.assertIn('tests/test_ha_esphome_requirements.txt', workflow)
        self.assertIn('make test-ha-esphome', workflow)
        self.assertIn('ESPHOME_HA_PYTHON:', workflow)
        self.assertNotIn('continue-on-error:', workflow)

    def test_requirements_pin_the_real_integration(self):
        requirements = (ROOT / 'tests/test_ha_esphome_requirements.txt').read_text().splitlines()
        self.assertIn('homeassistant==2026.9.4', requirements)
        self.assertIn('aioesphomeapi==46.2.0', requirements)
        for line in requirements:
            if line and not line.startswith('#'):
                self.assertRegex(line, r'^[A-Za-z0-9_.-]+==[^\s]+$')

    def test_normal_runner_checks_gate_wiring(self):
        runner = (ROOT / 'tests/run_tests.sh').read_text()
        self.assertIn('python3 tests/test_ha_esphome_wiring.py', runner)


if __name__ == '__main__':
    unittest.main()
