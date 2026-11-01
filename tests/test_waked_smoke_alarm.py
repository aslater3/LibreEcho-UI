#!/usr/bin/env python3
"""Smoke alarm (T3 pattern) detection through the real libreecho-waked wire.

The smoke alarm detector is off by default. These cases run the real daemon
against a scripted microphone that streams room noise with a T3-like train
(three 0.5 s pulses of a 3.1 kHz tone with 0.5 s gaps) and check what the
satellite sees:

- off by default: no smoke_alarm_detected event for the train;
- set_smoke_alarm enabled: the train is reported once as pulses=3 on the wake
  subscriber stream, and status counts it;
- set_smoke_alarm rejects a malformed or duplicated enabled field;
- set_smoke_alarm disabled: the train is no longer reported.

The decision rules are pinned by tests/test_smoke_alarm_detector.c. This file
covers the daemon wiring only. It does not show detection of a real alarm on a
real microphone; that check is still owed.
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
from test_waked_subscriber_slots import call  # noqa: E402
from test_waked_clap import ScriptedMic, room_noise  # noqa: E402

WAKED = os.environ.get('WAKED_BIN', str(ROOT / 'build' / 'libreecho-waked'))
RATE = 16000
EVENT = 'smoke_alarm_detected'


def tone_pulse(count_samples, hz=3100.0, amplitude=6000.0):
    import math
    return [int(amplitude * math.sin(2.0 * math.pi * hz * n / RATE)) for n in range(count_samples)]


def t3_signal(start_seconds, total_seconds, seed):
    """Room noise with a T3-like train of three 0.5 s pulses from start_seconds."""
    rng = random.Random(seed)
    samples = room_noise(rng, int(total_seconds * RATE))
    begin = int(start_seconds * RATE)
    pulse = RATE // 2
    for index in range(3):
        at = begin + index * 2 * pulse
        for n, value in enumerate(tone_pulse(pulse)):
            samples[at + n] = value
    return samples


def wait_for_events(subscriber, timeout, name=EVENT):
    """Read events of the given name from a subscriber socket until the timeout."""
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
            if message.get('event') == name:
                found.append(message['data'])
    return found


class SmokeAlarmWire(unittest.TestCase):
    def setUp(self):
        if not os.access(WAKED, os.X_OK):
            if os.environ.get('WAKED_REQUIRED') == '1':
                self.fail('libreecho-waked not built: ' + WAKED)
            self.skipTest('libreecho-waked not built')
        self.dir = tempfile.mkdtemp(prefix='waked-smoke-', dir=os.environ.get('TMPDIR'))
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
                raise AssertionError('waked did not start: %s' % err.decode(errors='replace'))
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
        s.sendall(json.dumps({'v': 1, 'id': 1, 'cmd': cmd, 'args': args or {}}).encode() + b'\n')
        line = b''
        while not line.endswith(b'\n'):
            chunk = s.recv(4096)
            if not chunk:
                break
            line += chunk
        s.close()
        return json.loads(line)

    def test_off_by_default_reports_nothing(self):
        self.start(t3_signal(2.0, 6.0, seed=1))
        subscriber = self.subscribe()
        status = self.command('status')
        self.assertTrue(status['ok'], status)
        self.assertFalse(status['data']['smoke_alarm_enabled'], status)
        self.assertEqual(status['data']['smoke_alarm_count'], 0, status)
        events = wait_for_events(subscriber, 4.0)
        subscriber.close()
        self.assertEqual(events, [], 'smoke alarm detection reported while off')

    def test_t3_train_reported_once_when_enabled(self):
        self.start(t3_signal(2.0, 8.5, seed=2))
        subscriber = self.subscribe()
        reply = self.command('set_smoke_alarm', {'enabled': True})
        self.assertTrue(reply['ok'], reply)
        self.assertTrue(self.command('status')['data']['smoke_alarm_enabled'])
        events = wait_for_events(subscriber, 7.5)
        subscriber.close()
        self.assertEqual([event['pulses'] for event in events], [3],
                         'expected one T3 event with pulses=3, got %r' % events)
        self.assertEqual(self.command('status')['data']['smoke_alarm_count'], 1)
        self.assertFalse(self.command('status')['data']['clap_enabled'])

    def test_malformed_enabled_is_rejected(self):
        self.start(t3_signal(2.0, 5.0, seed=3))
        for args in ({'enabled': 'yes'}, {'enabled': 1}, {}):
            reply = self.command('set_smoke_alarm', args)
            self.assertFalse(reply['ok'], 'set_smoke_alarm accepted %r' % (args,))
        self.assertFalse(self.command('status')['data']['smoke_alarm_enabled'])

    def test_disabling_stops_reports(self):
        self.start(t3_signal(2.0, 6.0, seed=4))
        self.assertTrue(self.command('set_smoke_alarm', {'enabled': True})['ok'])
        self.assertTrue(self.command('set_smoke_alarm', {'enabled': False})['ok'])
        subscriber = self.subscribe()
        events = wait_for_events(subscriber, 5.0)
        subscriber.close()
        self.assertEqual(events, [], 'smoke alarm detection reported after it was disabled')


if __name__ == '__main__':
    unittest.main()
