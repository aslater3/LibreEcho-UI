#!/usr/bin/env python3
"""Wake-path noise suppression through the real libreecho-waked wire.

The daemon removes steady noise before the VAD and the wake worker, where the
stock front end puts its Wiener reduction. These cases run the real daemon
against a scripted microphone and check the control and audio wire:

- boot default: suppression is on at a gentle attenuation, and `status` says
  so;
- `set_noise_suppression` rejects a malformed or duplicated `enabled` field;
- disabling it stops filtering, and enabling it again drops the RMS of a
  steady noise floor in the post-AEC audio stream.

The filter itself is pinned by tests/test_voice_ns.c; this file only covers
the daemon wiring and the audible effect.
"""
import json
import os
import pathlib
import random
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_waked_clap import ScriptedMic, room_noise  # noqa: E402
from test_waked_subscriber_slots import call  # noqa: E402

WAKED = os.environ.get('WAKED_BIN', str(ROOT / 'build' / 'libreecho-waked'))
MAGIC = 0x3153564C
HEADER = 24


def read_frames(sock, seconds):
    """Collect (first_sample, samples) audio frames for a wall-clock window."""
    frames = []
    buffer = b''
    deadline = time.time() + seconds
    sock.settimeout(0.2)
    while time.time() < deadline:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            break
        buffer += chunk
        while len(buffer) >= HEADER:
            magic, _version, _flags, first, count, _reserved = struct.unpack(
                '<IHHQII', buffer[:HEADER])
            if magic != MAGIC or count > 1280:
                raise AssertionError('bad voice stream header')
            size = HEADER + count * 2
            if len(buffer) < size:
                break
            payload = buffer[HEADER:size]
            buffer = buffer[size:]
            frames.append((first, struct.unpack('<%dh' % count, payload)))
    return frames


def rms_between(frames, rate, start_seconds, end_seconds):
    values = []
    for first, samples in frames:
        for index, sample in enumerate(samples):
            at = (first + index) / rate
            if start_seconds <= at < end_seconds:
                values.append(sample)
    if not values:
        raise AssertionError('no audio in the measured window')
    return (sum(v * v for v in values) / len(values)) ** 0.5


class WakeNoiseSuppression(unittest.TestCase):
    def setUp(self):
        if not os.access(WAKED, os.X_OK):
            if os.environ.get('WAKED_REQUIRED') == '1':
                self.fail('libreecho-waked not built: ' + WAKED)
            self.skipTest('libreecho-waked not built')
        self.dir = tempfile.mkdtemp(prefix='waked-ns-',
                                    dir=os.environ.get('TMPDIR'))
        self.mic = None
        self.proc = None

    def start(self, extra=()):
        rng = random.Random(11)
        samples = room_noise(rng, 16000 * 12)
        self.mic = ScriptedMic(os.path.join(self.dir, 'mic.sock'), samples)
        self.mic.start()
        self.sock = os.path.join(self.dir, 'wake.sock')
        self.proc = subprocess.Popen(
            [WAKED, '--foreground', '--socket', self.sock,
             '--mic-socket', self.mic.path,
             '--led-socket', os.path.join(self.dir, 'led.sock'),
             '--reference-socket', os.path.join(self.dir, 'ref.sock'),
             '--wake-threads', '1'] + list(extra),
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        deadline = time.time() + 5
        while not os.path.exists(self.sock):
            if self.proc.poll() is not None or time.time() > deadline:
                err = self.proc.stderr.read() if self.proc.stderr else b''
                raise AssertionError('waked did not start: %s'
                                     % err.decode(errors='replace'))
            time.sleep(.05)
        # Let the capture path settle before the first measurement.
        time.sleep(.4)

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

    def test_boot_default_is_on(self):
        self.start()
        status = self.command('status')['data']
        self.assertTrue(status['noise_suppression_enabled'], status)
        self.assertEqual(status['noise_suppression_db'], 6, status)
        self.assertGreater(status['noise_suppression_frames'], 0, status)

    def test_malformed_enabled_is_rejected(self):
        self.start()
        for args in ({'enabled': 'yes'}, {'enabled': 1}, {}):
            reply = self.command('set_noise_suppression', args)
            self.assertFalse(reply['ok'], 'accepted %r' % (args,))
        self.assertTrue(
            self.command('status')['data']['noise_suppression_enabled'])

    def test_enabled_suppresses_steady_noise(self):
        # Off at boot so the same run can measure both states.
        self.start(('--noise-suppression-db', '0'))
        status = self.command('status')['data']
        self.assertFalse(status['noise_suppression_enabled'], status)
        audio, reply = call(self.sock, 'stream_audio')
        self.assertTrue(reply['ok'], reply)
        off_frames = read_frames(audio, 2.0)
        off_rms = rms_between(off_frames, 16000, 0.5, 2.0)

        self.assertTrue(
            self.command('set_noise_suppression', {'enabled': True})['ok'])
        self.assertTrue(
            self.command('status')['data']['noise_suppression_enabled'])
        # Minimum statistics needs ~1.5 s to learn the floor; measure after.
        on_frames = read_frames(audio, 4.0)
        on_rms = rms_between(on_frames, 16000, 2.5, 4.0)
        audio.close()

        self.assertLess(on_rms, off_rms * 0.7,
                        'filtered RMS %.1f vs unfiltered %.1f'
                        % (on_rms, off_rms))
        self.assertTrue(
            self.command('set_noise_suppression',
                         {'enabled': False})['ok'])
        self.assertFalse(
            self.command('status')['data']['noise_suppression_enabled'])


if __name__ == '__main__':
    unittest.main()