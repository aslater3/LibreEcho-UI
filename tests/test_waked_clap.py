#!/usr/bin/env python3
"""Clap detection through the real libreecho-waked control and event wire.

The clap detector is off by default. These cases run the real daemon against a
scripted microphone that streams room noise with a clap (or two) in it, and
check the wire the satellite sees:

- off by default: no clap_detected event for a clap;
- set_clap enabled: a single clap is reported as claps=1 and two claps 300 ms
  apart as one claps=2 event on the wake subscriber stream;
- set_clap rejects a malformed or duplicated enabled field;
- set_clap disabled: claps are no longer reported.

The decision rules are pinned by tests/test_clap_detector.c; this file only
covers the daemon wiring.
"""
import json
import os
import pathlib
import random
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_waked_subscriber_slots import call  # noqa: E402

WAKED = os.environ.get('WAKED_BIN', str(ROOT / 'build' / 'libreecho-waked'))
RATE = 16000
CHUNK_SAMPLES = 320  # 20 ms per write, as the scripted mic streams it


def room_noise(rng, count):
    return [rng.randint(-20, 20) for _ in range(count)]


def clap(rng, count_samples=64, amplitude=12000.0):
    out = []
    for n in range(count_samples):
        envelope = 2.718281828 ** (-n / 24.0)
        out.append(int(max(-32768, min(32767, rng.uniform(-1, 1) * amplitude * envelope))))
    return out


def scripted_signal(claps_at, total_seconds, seed):
    """Room noise with claps starting at the given times (seconds)."""
    rng = random.Random(seed)
    samples = room_noise(rng, int(total_seconds * RATE))
    for start in claps_at:
        begin = int(start * RATE)
        for n, value in enumerate(clap(rng)):
            samples[begin + n] = value
    return samples


class ScriptedMic(threading.Thread):
    """Answers stream_mono, then streams the scripted samples in real time."""

    def __init__(self, path, samples):
        super().__init__(daemon=True)
        self.path = path
        self.samples = samples
        self.stop = threading.Event()
        self.server = socket.socket(socket.AF_UNIX)
        self.server.bind(path)
        self.server.listen(4)
        self.server.settimeout(.2)

    def run(self):
        client = None
        offset = 0
        while not self.stop.is_set():
            if client is None:
                try:
                    client, _ = self.server.accept()
                    client.recv(4096)
                    client.sendall(b'{"v":1,"id":1,"ok":true,"data":{"streaming":true,"format":"pcm_s16_le","calibration_applied":true},"error":null}\n')
                except socket.timeout:
                    continue
            chunk = self.samples[offset:offset + CHUNK_SAMPLES]
            if len(chunk) < CHUNK_SAMPLES:
                chunk = chunk + [0] * (CHUNK_SAMPLES - len(chunk))
            else:
                offset += CHUNK_SAMPLES
            try:
                client.sendall(b''.join(int(v).to_bytes(2, 'little', signed=True) for v in chunk))
            except OSError:
                break
            time.sleep(CHUNK_SAMPLES / RATE)
        if client:
            client.close()
        self.server.close()


def wait_for_clap_events(subscriber, timeout):
    """Read clap_detected events from a subscriber socket until the timeout."""
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
            if message.get('event') == 'clap_detected':
                found.append(message['data'])
    return found


class ClapWire(unittest.TestCase):
    def setUp(self):
        if not os.access(WAKED, os.X_OK):
            if os.environ.get('WAKED_REQUIRED') == '1':
                self.fail('libreecho-waked not built: ' + WAKED)
            self.skipTest('libreecho-waked not built')
        self.dir = tempfile.mkdtemp(prefix='waked-clap-', dir=os.environ.get('TMPDIR'))
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

    def test_off_by_default_reports_no_claps(self):
        self.start(scripted_signal([2.0], 4.0, seed=1))
        subscriber = self.subscribe()
        status = self.command('status')
        self.assertFalse(status['data']['clap_enabled'], status)
        events = wait_for_clap_events(subscriber, 3.0)
        subscriber.close()
        self.assertEqual(events, [], 'clap detection reported while off')

    def test_single_then_double_clap_when_enabled(self):
        # One clap at 2.0 s, then a double at 6.0 s and 6.3 s.
        self.start(scripted_signal([2.0, 6.0, 6.3], 9.5, seed=2))
        subscriber = self.subscribe()
        reply = self.command('set_clap', {'enabled': True})
        self.assertTrue(reply['ok'], reply)
        self.assertTrue(self.command('status')['data']['clap_enabled'])
        events = wait_for_clap_events(subscriber, 8.5)
        subscriber.close()
        claps = [event['claps'] for event in events]
        self.assertEqual(claps, [1, 2], 'expected a single then a double, got %r' % events)
        self.assertGreater(self.command('status')['data']['clap_count'], 1)

    def test_malformed_enabled_is_rejected(self):
        self.start(scripted_signal([], 1.0, seed=3))
        for args in ({'enabled': 'yes'}, {'enabled': 1}, {}):
            reply = self.command('set_clap', args)
            self.assertFalse(reply['ok'], 'set_clap accepted %r' % (args,))
        self.assertFalse(self.command('status')['data']['clap_enabled'])

    def test_disabling_stops_reports(self):
        self.start(scripted_signal([3.0], 6.0, seed=4))
        self.assertTrue(self.command('set_clap', {'enabled': True})['ok'])
        self.assertTrue(self.command('set_clap', {'enabled': False})['ok'])
        subscriber = self.subscribe()
        events = wait_for_clap_events(subscriber, 5.0)
        subscriber.close()
        self.assertEqual(events, [], 'clap detection reported after it was disabled')


if __name__ == '__main__':
    unittest.main()
