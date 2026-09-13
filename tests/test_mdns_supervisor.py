"""Host-isolated supervisor tests; fake children do not prove Avahi publication."""
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import unittest
import sys

ROOT = Path(__file__).resolve().parents[1]

class Supervisor(unittest.TestCase):
    def test_lease_eof_and_single_supervisor(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            services = root / 'etc/avahi/services'
            services.mkdir(parents=True)
            (root / 'run/dbus').mkdir(parents=True)
            fake = root / 'chroot'
            fake.write_text('#!/bin/sh\ntrap "exit 0" TERM INT\ntrap : HUP\nwhile :; do sleep 0.05; done\n')
            fake.chmod(0o755)
            binary = root / 'mdnsd'
            compile_result = subprocess.run([
                'cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                '-DMDNS_CHROOT="' + str(fake) + '"',
                '-DMDNS_OWNER_EXE="' + os.path.realpath(sys.executable) + '"',
                str(ROOT / 'src/adapter/mdnsd.c'),
                str(ROOT / 'src/adapter/mdns_lease.c'), '-o', str(binary)],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            path = root / 'control.sock'
            command = [str(binary), '--root', str(root), '--socket', str(path)]
            proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 5
                while not path.exists() and time.monotonic() < deadline:
                    self.assertIsNone(proc.poll())
                    time.sleep(0.02)
                self.assertTrue(path.exists())
                duplicate = subprocess.run(command, capture_output=True, timeout=5)
                self.assertNotEqual(duplicate.returncode, 0)
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as owner:
                    owner.settimeout(3)
                    owner.connect(str(path))
                    owner.sendall(b'WYOMING/1 21000\n')
                    self.assertEqual(owner.recv(128), b'pending\n')
                    files = list(services.glob('*.service'))
                    self.assertEqual(len(files), 1)
                    self.assertIn('<port>21000</port>', files[0].read_text())
                    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as other:
                        other.settimeout(3)
                        other.connect(str(path))
                        other.sendall(b'WYOMING/1 10700\n')
                        self.assertEqual(other.recv(128), b'error\n')
                deadline = time.monotonic() + 3
                while list(services.glob('*.service')) and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertFalse(list(services.glob('*.service')))
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as invalid:
                    invalid.settimeout(3)
                    invalid.connect(str(path))
                    invalid.sendall(b'WYOMING/1 65536\n')
                    self.assertEqual(invalid.recv(128), b'error\n')
            finally:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.communicate(timeout=5)
                    raise
            self.assertEqual(proc.returncode, 0)
            self.assertFalse(path.exists())

if __name__ == '__main__':
    unittest.main()
