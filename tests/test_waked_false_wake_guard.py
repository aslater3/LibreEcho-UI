#!/usr/bin/env python3
"""False-wake prevention through the real libreecho-waked wire.

The guard refuses a detection that arrives while the capture chain is clipped
or silent, unless the model score is strong. These cases run the real daemon
against a scripted microphone and use the `test` command, which settles a
synthetic wake through the same path as a real detection:

- boot default: the guard is on and `status` says so;
- a detection during a clipped stretch is refused and counted, while the same
  detection after the overload clears is published;
- disabling the guard lets the clipped detection through;
- a malformed or duplicated `enabled` field is rejected.

The rules themselves are pinned by tests/test_false_wake_guard.c.
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
from test_waked_clap import ScriptedMic, RATE  # noqa: E402
from test_waked_subscriber_slots import call  # noqa: E402

WAKED = os.environ.get('WAKED_BIN', str(ROOT / 'build' / 'libreecho-waked'))


def signal(clipped_seconds=2.0, clean_seconds=4.0):
    rng = random.Random(23)
    samples = []
    for value in range(int(clipped_seconds * RATE)):
        samples.append(32767 if value % 2 else -32768)
    for _ in range(int(clean_seconds * RATE)):
        samples.append(rng.randint(-800, 800))
    return samples


class FalseWakeGuardWire(unittest.TestCase):
    def setUp(self):
        if not os.access(WAKED, os.X_OK):
            if os.environ.get('WAKED_REQUIRED') == '1':
                self.fail('libreecho-waked not built: ' + WAKED)
            self.skipTest('libreecho-waked not built')
        self.dir = tempfile.mkdtemp(prefix='waked-fw-',
                                    dir=os.environ.get('TMPDIR'))
        self.mic = None
        self.proc = None

    def start(self):
        self.mic = ScriptedMic(os.path.join(self.dir, 'mic.sock'),
                               signal())
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

    def wake_events(self, subscriber, timeout):
        found = []
        subscriber.settimeout(0.2)
        buffer = b''
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                chunk = subscriber.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            buffer += chunk
            while b'\n' in buffer:
                line, buffer = buffer.split(b'\n', 1)
                try:
                    message = json.loads(line)
                except ValueError:
                    continue
                if message.get('event') == 'wake_detected':
                    found.append(message['data'])
        return found

    def test_clipped_detection_is_refused_then_clean_one_passes(self):
        self.start()
        subscriber, reply = call(self.sock, 'subscribe')
        self.assertTrue(reply['ok'], reply)
        status = self.command('status')['data']
        self.assertTrue(status['false_wake_guard_enabled'], status)

        time.sleep(1.0)  # inside the clipped stretch
        self.command('test')
        refused = self.wake_events(subscriber, 1.0)
        self.assertEqual(refused, [], 'clipped detection was published')
        self.assertGreaterEqual(
            self.command('status')['data']['false_wake_guard_count'], 1)

        # The guard looks at the second before the detection; wait for the
        # overload to leave that window entirely.
        time.sleep(2.4)
        self.command('test')
        accepted = self.wake_events(subscriber, 1.0)
        subscriber.close()
        self.assertEqual(len(accepted), 1, accepted)

    def test_disabled_guard_accepts_clipped_detection(self):
        self.start()
        self.assertTrue(
            self.command('set_false_wake_guard', {'enabled': False})['ok'])
        self.assertFalse(
            self.command('status')['data']['false_wake_guard_enabled'])
        subscriber, reply = call(self.sock, 'subscribe')
        self.assertTrue(reply['ok'], reply)
        time.sleep(1.0)
        self.command('test')
        accepted = self.wake_events(subscriber, 1.0)
        subscriber.close()
        self.assertEqual(len(accepted), 1, accepted)

    def test_malformed_enabled_is_rejected(self):
        self.start()
        for args in ({'enabled': 'yes'}, {'enabled': 1}, {}):
            reply = self.command('set_false_wake_guard', args)
            self.assertFalse(reply['ok'], 'accepted %r' % (args,))
        self.assertTrue(
            self.command('status')['data']['false_wake_guard_enabled'])


if __name__ == '__main__':
    unittest.main()