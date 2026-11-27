#!/usr/bin/env python3
"""Residual echo suppression through the real libreecho-waked wire.

The suppressor runs inside the echo canceller, so the DSP behaviour is pinned
by tests/test_voice_aec_residual.c. These cases cover the daemon wiring:

- the stock-style default is on, at -40 dB when only echo remains and -10 dB
  while near-end speech is present, and `status` says so;
- `set_residual_echo` round-trips and rejects a malformed or duplicated
  `enabled` field.
"""
import json
import os
import pathlib
import random
import socket
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_waked_clap import ScriptedMic, room_noise  # noqa: E402

WAKED = os.environ.get('WAKED_BIN', str(ROOT / 'build' / 'libreecho-waked'))


class ResidualEchoWire(unittest.TestCase):
    def setUp(self):
        if not os.access(WAKED, os.X_OK):
            if os.environ.get('WAKED_REQUIRED') == '1':
                self.fail('libreecho-waked not built: ' + WAKED)
            self.skipTest('libreecho-waked not built')
        self.dir = tempfile.mkdtemp(prefix='waked-res-',
                                    dir=os.environ.get('TMPDIR'))
        self.mic = None
        self.proc = None

    def start(self):
        rng = random.Random(5)
        self.mic = ScriptedMic(os.path.join(self.dir, 'mic.sock'),
                               room_noise(rng, 16000 * 6))
        self.mic.start()
        self.sock = os.path.join(self.dir, 'wake.sock')
        self.proc = subprocess.Popen(
            [WAKED, '--foreground', '--socket', self.sock,
             '--mic-socket', self.mic.path,
             '--led-socket', os.path.join(self.dir, 'led.sock'),
             '--reference-socket', os.path.join(self.dir, 'ref.sock'),
             '--wake-threads', '1'],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        deadline = time.time() + 5
        while not os.path.exists(self.sock):
            if self.proc.poll() is not None or time.time() > deadline:
                err = self.proc.stderr.read() if self.proc.stderr else b''
                raise AssertionError('waked did not start: %s'
                                     % err.decode(errors='replace'))
            time.sleep(.05)

    def tearDown(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(5)
        if self.proc and self.proc.stderr:
            self.proc.stderr.close()
        if self.mic:
            self.mic.stop.set()
            self.mic.join(2)

    def command(self, cmd, args=None, timeout=3.0):
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(timeout)
        s.connect(self.sock)
        s.sendall(json.dumps({'v': 1, 'id': 1, 'cmd': cmd,
                              'args': args or {}}).encode() + b'\n')
        line = b''
        while not line.endswith(b'\n'):
            chunk = s.recv(4096)
            if not chunk:
                break
            line += chunk
        s.close()
        return json.loads(line)

    def test_defaults_are_stock_style(self):
        self.start()
        status = self.command('status')['data']
        self.assertTrue(status['residual_echo_enabled'], status)
        self.assertEqual(status['residual_echo_suppress_db'], -40, status)
        self.assertEqual(status['residual_echo_active_db'], -10, status)

    def test_toggle_round_trips(self):
        self.start()
        self.assertTrue(
            self.command('set_residual_echo', {'enabled': False})['ok'])
        status = self.command('status')['data']
        self.assertFalse(status['residual_echo_enabled'], status)
        self.assertTrue(
            self.command('set_residual_echo', {'enabled': True})['ok'])
        status = self.command('status')['data']
        self.assertTrue(status['residual_echo_enabled'], status)
        self.assertEqual(status['residual_echo_suppress_db'], -40, status)

    def test_malformed_enabled_is_rejected(self):
        self.start()
        for args in ({'enabled': 'yes'}, {'enabled': 1}, {}):
            reply = self.command('set_residual_echo', args)
            self.assertFalse(reply['ok'], 'accepted %r' % (args,))
        self.assertTrue(
            self.command('status')['data']['residual_echo_enabled'])


if __name__ == '__main__':
    unittest.main()
