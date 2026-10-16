"""Historical runner entry: discovery now follows a live ESPHome lease only.

Old Wyoming PORT/ARGS/mode rendering is deliberately gone. Exercise shipped
init cleanup in scratch directories, plus the reference ESPHome TXT schema.
Actual port/bounds/rendering/socket ownership is covered by test_mdns_esphome.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
sys.dont_write_bytecode = True
from test_mdns_esphome import FIELDS

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'init/libreecho-mdnsd.init'

class Discovery(unittest.TestCase):
    def test_cold_boot_cleanup_never_synthesizes_a_listener(self):
        match = re.search(r'^clear_ha_services\(\) \{\n.*?^\}', SCRIPT.read_text(), re.M | re.S)
        self.assertIsNotNone(match, 'init needs lease-only HA cleanup')
        for integrations in [0, 1, 16, 17]:
            for mode in ['local', 'custom', 'home-assistant']:
                with self.subTest(integrations=integrations, mode=mode), tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    services = root / 'etc/avahi/services'
                    services.mkdir(parents=True)
                    config = root / 'config.json'
                    config.write_text('{"integrations":%d,"voice_pipeline_mode":"%s"}' % (integrations, mode))
                    defaults = root / 'defaults'
                    defaults.write_text('PORT=12345\nARGS="--port 23456"\n')
                    for name in ['wyoming.service', 'wyoming-2.service', 'wyoming-3.tmp',
                                 'esphome.service', 'esphome-1.service', 'esphome-2.tmp']:
                        (services / name).write_text('stale')
                    airplay = services / 'airplay.service'
                    airplay.write_text('preserved')
                    result = subprocess.run(['sh', '-c', match.group() + '\nclear_ha_services\n'],
                        env=dict(os.environ, ROOT=str(root), CONFIG=str(config), WYOMING_DEFAULTS=str(defaults)),
                        capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(list(services.iterdir()), [airplay])
                    self.assertEqual(airplay.read_text(), 'preserved')

    def test_cleanup_rejects_directory_record_without_deleting_its_contents(self):
        match = re.search(r'^clear_ha_services\(\) \{\n.*?^\}', SCRIPT.read_text(), re.M | re.S)
        self.assertIsNotNone(match)
        with tempfile.TemporaryDirectory() as tmp:
            services = Path(tmp) / 'etc/avahi/services'
            directory = services / 'esphome-1.service'
            directory.mkdir(parents=True)
            child = directory / 'keep'
            child.write_text('preserve')
            result = subprocess.run(['sh', '-c', match.group() + '\nclear_ha_services\n'],
                env=dict(os.environ, ROOT=tmp), capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(child.read_text(), 'preserve')

    def test_reference_schema_is_esphome_only_and_never_a_real_identity(self):
        schema = ET.parse(ROOT / 'config/esphome.service').getroot()
        self.assertEqual(schema.findtext('service/type'), '_esphomelib._tcp')
        self.assertEqual(schema.findtext('service/port'), '6053')
        fields = dict(node.text.split('=', 1) for node in schema.findall('service/txt-record'))
        self.assertEqual(set(fields), set(FIELDS))
        self.assertEqual(fields['api_encryption'], FIELDS['api_encryption'])
        self.assertEqual(fields['mac'], '@MAC@')
        self.assertEqual(schema.find('name').attrib['replace-wildcards'], 'no')

if __name__ == '__main__':
    unittest.main()
