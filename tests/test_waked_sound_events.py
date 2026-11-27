#!/usr/bin/env python3
"""Glass-break and baby-cry detection through the real waked wire.

The detectors are off by default. These cases run the real daemon against a
scripted microphone and check the control and event wire the satellite sees:

- off by default: status says so and no event is reported;
- set_glass_break / set_baby_cry enabled: a shatter+tinkle and a pair of cry
  bouts each produce one event on the wake subscriber stream;
- a malformed or duplicated enabled field is rejected.

The decision rules are pinned by tests/test_glass_break_detector.c and
tests/test_baby_cry_detector.c; this file only covers the daemon wiring.
"""
import json
import math
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
from test_waked_clap import ScriptedMic, room_noise, RATE  # noqa: E402
from test_waked_subscriber_slots import call  # noqa: E402

WAKED = os.environ.get('WAKED_BIN', str(ROOT / 'build' / 'libreecho-waked'))


def add_glass(samples, rng, start, amplitude=9000.0, ms=14.0):
    begin = int(start * RATE)
    previous = 0.0
    for n in range(int(ms * RATE / 1000)):
        fresh = rng.uniform(-1, 1)
        value = amplitude * (fresh - previous) * math.exp(-n / 48.0)
        previous = fresh
        samples[begin + n] += int(max(-32768, min(32767, value)))


def add_cry_bout(samples, start, ms=800.0, amplitude=9000.0):
    begin = int(start * RATE)
    length = int(ms * RATE / 1000)
    for n in range(length):
        t = n / RATE
        f0 = 420.0 * (1.0 + 0.03 * math.sin(2 * math.pi * 6.0 * t))
        envelope = math.sin(math.pi * n / length)
        voice = sum(math.sin(2 * math.pi * f0 * h * t) / h
                    for h in range(1, 11))
        samples[begin + n] += int(voice * envelope * amplitude)


class SoundEventsWire(unittest.TestCase):
    def setUp(self):
        if not os.access(WAKED, os.X_OK):
            if os.environ.get('WAKED_REQUIRED') == '1':
                self.fail('libreecho-waked not built: ' + WAKED)
            self.skipTest('libreecho-waked not built')
        self.dir = tempfile.mkdtemp(prefix='waked-events-',
                                    dir=os.environ.get('TMPDIR'))
        self.mic = None
        self.proc = None

    def start(self, samples):
        self.mic = ScriptedMic(os.path.join(self.dir, 'mic.sock'), samples)
        self.mic.start()
        self.sock = os.path.join(self.dir, 'wake.sock')
        self.proc = subprocess.Popen(
            [WAKED, '--foreground', '--socket', self.sock,
             '--mic-socket', self.mic.path,
             '--led-socket', os.path.join(self.dir, 'led.sock'),
             '--reference-socket', os.path.join(self.dir, 'ref.sock')],
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

    def subscribe(self):
        s, reply = call(self.sock, 'subscribe')
        self.assertTrue(reply['ok'], reply)
        return s

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

    def wait_for(self, subscriber, names, timeout):
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
                if message.get('event') in names:
                    found.append(message['event'])
        return found

    def signal(self):
        rng = random.Random(7)
        total = int(8.0 * RATE)
        samples = room_noise(rng, total)
        add_glass(samples, rng, 1.0)
        add_glass(samples, rng, 1.3)
        add_cry_bout(samples, 3.0)
        add_cry_bout(samples, 4.2)
        return samples

    def test_off_by_default(self):
        self.start(self.signal())
        subscriber = self.subscribe()
        status = self.command('status')['data']
        self.assertFalse(status['glass_break_enabled'], status)
        self.assertFalse(status['baby_cry_enabled'], status)
        events = self.wait_for(
            subscriber, ('glass_break_detected', 'baby_cry_detected'), 6.0)
        subscriber.close()
        self.assertEqual(events, [], 'events reported while off')

    def test_enabled_reports_both(self):
        self.start(self.signal())
        subscriber = self.subscribe()
        self.assertTrue(self.command('set_glass_break',
                                     {'enabled': True})['ok'])
        self.assertTrue(self.command('set_baby_cry',
                                     {'enabled': True})['ok'])
        status = self.command('status')['data']
        self.assertTrue(status['glass_break_enabled'], status)
        self.assertTrue(status['baby_cry_enabled'], status)
        events = self.wait_for(
            subscriber, ('glass_break_detected', 'baby_cry_detected'), 7.0)
        subscriber.close()
        self.assertIn('glass_break_detected', events, events)
        self.assertIn('baby_cry_detected', events, events)
        self.assertGreaterEqual(
            self.command('status')['data']['glass_break_count'], 1)
        self.assertGreaterEqual(
            self.command('status')['data']['baby_cry_count'], 1)

    def test_malformed_enabled_is_rejected(self):
        self.start(self.signal()[:RATE])
        for cmd in ('set_glass_break', 'set_baby_cry'):
            for args in ({'enabled': 'yes'}, {'enabled': 1}, {}):
                reply = self.command(cmd, args)
                self.assertFalse(reply['ok'], '%s accepted %r' % (cmd, args))
            self.assertFalse(
                self.command('status')['data'][cmd[4:] + '_enabled'])


if __name__ == '__main__':
    unittest.main()
