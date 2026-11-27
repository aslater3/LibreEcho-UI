#!/usr/bin/env python3
"""Late-reverberation suppression through the real libreecho-waked wire.

The suppressor itself is pinned by tests/test_voice_dereverb.c. These cases
cover the daemon wiring:

- the stock-style default is on at 12 dB, and `status` says so;
- `set_dereverb` round-trips, including 0 for off and the 3..24 clamp range;
- a malformed, fractional or out-of-range `db` is rejected;
- the processed-frame counter advances while the stage runs.

The 12 dB default is a judgement call from the stock CDR Dereverberator, not a
value measured on this hardware.
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


class DereverbWire(unittest.TestCase):
    def setUp(self):
        if not os.access(WAKED, os.X_OK):
            if os.environ.get('WAKED_REQUIRED') == '1':
                self.fail('libreecho-waked not built: ' + WAKED)
            self.skipTest('libreecho-waked not built')
        self.dir = tempfile.mkdtemp(prefix='waked-dereverb-',
                                    dir=os.environ.get('TMPDIR'))
        self.mic = None
        self.proc = None

    def start(self, extra=None):
        rng = random.Random(7)
        self.mic = ScriptedMic(os.path.join(self.dir, 'mic.sock'),
                               room_noise(rng, 16000 * 6))
        self.mic.start()
        self.sock = os.path.join(self.dir, 'wake.sock')
        args = [WAKED, '--foreground', '--socket', self.sock,
                '--mic-socket', self.mic.path,
                '--led-socket', os.path.join(self.dir, 'led.sock'),
                '--reference-socket', os.path.join(self.dir, 'ref.sock'),
                '--wake-threads', '1']
        if extra:
            args += extra
        self.proc = subprocess.Popen(
            args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
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
        self.assertTrue(status['dereverb_enabled'], status)
        self.assertEqual(status['dereverb_db'], 12, status)

    def test_toggle_round_trips(self):
        self.start()
        self.assertTrue(self.command('set_dereverb', {'db': 0})['ok'])
        status = self.command('status')['data']
        self.assertFalse(status['dereverb_enabled'], status)
        self.assertEqual(status['dereverb_db'], 0, status)
        self.assertTrue(self.command('set_dereverb', {'db': 18})['ok'])
        status = self.command('status')['data']
        self.assertTrue(status['dereverb_enabled'], status)
        self.assertEqual(status['dereverb_db'], 18, status)
        # Levels 1 and 2 are clamped to the 3 dB minimum, as on the command
        # line, so status always reports the level in use.
        self.assertTrue(self.command('set_dereverb', {'db': 1})['ok'])
        self.assertEqual(self.command('status')['data']['dereverb_db'], 3)

    def test_malformed_db_is_rejected(self):
        self.start()
        for args in ({'db': 'twelve'}, {'db': 1.5}, {}, {'db': 25},
                     {'db': -1}, {'db': True}, {'db': 5, 'level': 2}):
            reply = self.command('set_dereverb', args)
            self.assertFalse(reply['ok'], 'accepted %r' % (args,))
        status = self.command('status')['data']
        self.assertTrue(status['dereverb_enabled'], status)
        self.assertEqual(status['dereverb_db'], 12, status)

    def test_frames_advance_while_enabled(self):
        self.start(['--dereverb-db', '12'])
        first = self.command('status')['data']['dereverb_frames']
        time.sleep(.5)
        second = self.command('status')['data']['dereverb_frames']
        self.assertGreater(second, first)


if __name__ == '__main__':
    unittest.main()

