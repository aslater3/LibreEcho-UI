"""Real Wyoming listener lifecycle against the local discovery supervisor."""
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]

def await_condition(test, condition, seconds=4):
    deadline = time.monotonic() + seconds
    while not condition() and time.monotonic() < deadline:
        time.sleep(0.02)
    test.assertTrue(condition())

class Wyoming(unittest.TestCase):
    def test_live_listener_owns_registration(self):
        subprocess.run(['make', 'build/libreecho-wyomingd'], cwd=ROOT,
                       check=True, capture_output=True, timeout=60)
        wyoming = ROOT / 'build/libreecho-wyomingd'
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            services = root / 'etc/avahi/services'
            services.mkdir(parents=True)
            (root / 'run').mkdir()
            fake = root / 'chroot'
            fake.write_text('#!/bin/sh\ntrap "exit 0" TERM INT\ntrap : HUP\nwhile :; do sleep 0.05; done\n')
            fake.chmod(0o755)
            mdns = root / 'mdnsd'
            subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                            '-DMDNS_CHROOT="' + str(fake) + '"',
                            '-DMDNS_OWNER_EXE="' + str(wyoming) + '"',
                            str(ROOT / 'src/adapter/mdnsd.c'),
                            str(ROOT / 'src/adapter/mdns_lease.c'), '-o', str(mdns)],
                           check=True, capture_output=True, timeout=30)
            control = root / 'mdns.sock'
            supervisor = subprocess.Popen([str(mdns), '--root', str(root), '--socket', str(control)])
            consumer = None
            try:
                await_condition(self, control.exists)
                with socket.socket() as probe:
                    probe.bind(('127.0.0.1', 0))
                    port = probe.getsockname()[1]
                consumer = subprocess.Popen([str(wyoming), '--port', str(port),
                    '--mdns-socket', str(control), '--wake-socket', str(root / 'absent')],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                await_condition(self, lambda: bool(list(services.glob('*.service'))))
                self.assertIsNone(consumer.poll())
                self.assertIn(f'<port>{port}</port>', next(services.glob('*.service')).read_text())
                with socket.create_connection(('127.0.0.1', port), timeout=2):
                    pass
                consumer.terminate(); consumer.communicate(timeout=3)
                self.assertEqual(consumer.returncode, 0)
                await_condition(self, lambda: not list(services.glob('*.service')))
            finally:
                if consumer and consumer.poll() is None:
                    consumer.terminate(); consumer.communicate(timeout=3)
                supervisor.terminate(); supervisor.wait(timeout=3)

if __name__ == '__main__':
    unittest.main()
