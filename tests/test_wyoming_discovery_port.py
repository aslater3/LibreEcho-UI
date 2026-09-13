"""Exercise the shipped Avahi service rendering with no host mounts.

A review finding showed config/wyoming.service always advertised port 10700,
even though libreecho-wyomingd honours a PORT override from
/etc/default/libreecho-wyomingd. This runs the real prepare_avahi_runtime()
and wyoming_service_port() helpers extracted from the shipped init script and
asserts the generated runtime service uses the effective Wyoming port.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = Path(os.environ.get('SCRIPT', ROOT / 'init/libreecho-airplayd.init'))
SERVICE_TEMPLATE = ROOT / 'config/wyoming.service'


def extract(source, names):
    helpers = []
    for name in names:
        found = re.search(r'^' + name + r'\(\) \{\n.*?^\}', source, re.M | re.S)
        if not found:
            raise AssertionError(f'helper {name} not found in {SCRIPT}')
        helpers.append(found.group())
    return '\n'.join(helpers)


class WyomingDiscoveryPort(unittest.TestCase):
    def run_case(self, defaults=None, enabled='1', preexisting=False):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            runtime = root / 'runtime'
            (runtime / 'etc/avahi/services').mkdir(parents=True)
            source = root / 'wyoming.service'
            source.write_text(SERVICE_TEMPLATE.read_text())
            generated = runtime / 'etc/avahi/services/wyoming.service'
            if preexisting:
                generated.write_text('<service-group/>\n')
            defaults_path = root / 'libreecho-wyomingd'
            if defaults is not None:
                defaults_path.write_text(defaults)
            program = extract(SCRIPT.read_text(),
                              ['wyoming_service_port', 'prepare_avahi_runtime'])
            program += '\nprepare_avahi_runtime\n'
            env = dict(os.environ,
                       RUNTIME_ROOT=str(runtime),
                       WYOMING_SERVICE_SOURCE=str(source),
                       WYOMING_DEFAULTS=str(defaults_path),
                       AVAHI_SERVICES_SOURCE=str(root / 'absent'),
                       HOME_ASSISTANT_ENABLED=enabled)
            proc = subprocess.run(['sh', '-c', program], env=env,
                                  capture_output=True, text=True, timeout=5)
            body = generated.read_text() if generated.exists() else None
            return proc, body

    def test_port_override_is_advertised(self):
        proc, body = self.run_case(defaults='PORT=12345\n')
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIsNotNone(body)
        self.assertIn('<port>12345</port>', body)
        self.assertNotIn('<port>10700</port>', body)

    def test_quoted_override_is_advertised(self):
        proc, body = self.run_case(defaults='PORT="12000"\n')
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIsNotNone(body)
        self.assertIn('<port>12000</port>', body)

    def test_default_port_without_defaults_file(self):
        proc, body = self.run_case(defaults=None)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIsNotNone(body)
        self.assertIn('<port>10700</port>', body)

    def test_non_numeric_override_falls_back_to_default(self):
        proc, body = self.run_case(defaults='PORT=not-a-port\n')
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIsNotNone(body)
        self.assertIn('<port>10700</port>', body)

    def test_empty_override_falls_back_to_default(self):
        proc, body = self.run_case(defaults='PORT=\n')
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIsNotNone(body)
        self.assertIn('<port>10700</port>', body)

    def test_disabled_home_assistant_removes_service(self):
        proc, body = self.run_case(defaults='PORT=12345\n', enabled='0',
                                   preexisting=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIsNone(body)


if __name__ == '__main__':
    unittest.main()
