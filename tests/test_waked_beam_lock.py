#!/usr/bin/env python3
"""Wake-direction beam lock through the real libreecho-waked wire.

micd owns the beamformer; waked hears the wake word on the selected beam and
must pin the array there for the follow-up. These cases use a fake micd that
serves both the mono stream and the control socket, and check that:

- a wake ("listen" here, which publishes the same event) sends lock_beam with
  the configured hold;
- --beam-lock-ms 0 sends nothing;
- a custom hold is passed through.

The beamformer side is pinned by tests/test_micd.c.
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
from test_waked_clap import room_noise  # noqa: E402

WAKED = os.environ.get('WAKED_BIN', str(ROOT / 'build' / 'libreecho-waked'))
HEADER = (b'{"v":1,"id":1,"ok":true,"data":{"streaming":true,'
          b'"format":"pcm_s16_le","calibration_applied":true},'
          b'"error":null}\n')


class FakeMicd(threading.Thread):
    """A micd stand-in: streams silence and records lock_beam requests."""

    def __init__(self, path):
        super().__init__(daemon=True)
        self.path = path
        self.stop = threading.Event()
        self.locks = []
        self.lock = threading.Lock()
        self.server = socket.socket(socket.AF_UNIX)
        self.server.bind(path)
        self.server.listen(8)
        self.server.settimeout(.2)
        self.samples = room_noise(random.Random(3), 16000 * 30)

    def run(self):
        while not self.stop.is_set():
            try:
                client, _ = self.server.accept()
            except socket.timeout:
                continue
            threading.Thread(target=self.handle, args=(client,),
                             daemon=True).start()

    def handle(self, client):
        data = b''
        client.settimeout(2)
        try:
            while b'\n' not in data:
                chunk = client.recv(4096)
                if not chunk:
                    return
                data += chunk
            request = json.loads(data.split(b'\n', 1)[0])
        except (OSError, ValueError):
            client.close()
            return
        if request.get('cmd') == 'stream_mono':
            try:
                client.sendall(HEADER)
                offset = 0
                while not self.stop.is_set():
                    chunk = self.samples[offset:offset + 320]
                    offset = (offset + 320) % len(self.samples)
                    client.sendall(b''.join(
                        int(v).to_bytes(2, 'little', signed=True)
                        for v in chunk))
                    time.sleep(0.02)
            except OSError:
                pass
        else:
            if request.get('cmd') == 'lock_beam':
                with self.lock:
                    self.locks.append(request.get('args') or {})
            try:
                client.sendall(json.dumps({
                    'v': 1, 'id': request.get('id', 1), 'ok': True,
                    'data': {}, 'error': None}).encode() + b'\n')
            except OSError:
                pass
        client.close()


class BeamLock(unittest.TestCase):
    def setUp(self):
        if not os.access(WAKED, os.X_OK):
            if os.environ.get('WAKED_REQUIRED') == '1':
                self.fail('libreecho-waked not built: ' + WAKED)
            self.skipTest('libreecho-waked not built')
        self.dir = tempfile.mkdtemp(prefix='waked-beamlock-',
                                    dir=os.environ.get('TMPDIR'))
        self.mic = None
        self.proc = None

    def start(self, extra=None):
        self.mic = FakeMicd(os.path.join(self.dir, 'mic.sock'))
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
        time.sleep(.2)  # let the mono stream start

    def tearDown(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(5)
        if self.proc and self.proc.stderr:
            self.proc.stderr.close()
        if self.mic:
            self.mic.stop.set()
            self.mic.join(2)
            self.mic.server.close()

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

    def wait_for_lock(self, timeout=2.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.mic.lock:
                if self.mic.locks:
                    return self.mic.locks[0]
            time.sleep(.02)
        raise AssertionError('no lock_beam request arrived')

    def test_wake_pins_the_default_hold(self):
        self.start()
        self.assertTrue(self.command('listen')['ok'])
        self.assertEqual(self.wait_for_lock()['hold_ms'], 8000)

    def test_zero_disables_the_lock(self):
        self.start(['--beam-lock-ms', '0'])
        self.assertTrue(self.command('listen')['ok'])
        time.sleep(.4)
        with self.mic.lock:
            self.assertEqual(self.mic.locks, [])

    def test_custom_hold_is_passed_through(self):
        self.start(['--beam-lock-ms', '4500'])
        self.assertTrue(self.command('listen')['ok'])
        self.assertEqual(self.wait_for_lock()['hold_ms'], 4500)


if __name__ == '__main__':
    unittest.main()
