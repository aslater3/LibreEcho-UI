#!/usr/bin/env python3
"""A subscriber that hangs up must give its waked slot back.

waked keeps four wake-event slots and two audio slots. A slot used to be freed
only when a send to it failed, and sends happen only on a wake event (or, for
audio, while frames flow). A client that subscribed and then went away while
the room was quiet kept its slot indefinitely; on hardware a duplicate agentd
and one test listener were enough to make every later subscriber fail with
"subscriber limit reached", including esphomed's wake path.

Runs the real daemon against a fake microphone that streams silence.
"""
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
WAKED = os.environ.get('WAKED_BIN', str(ROOT / 'build' / 'libreecho-waked'))


class FakeMic(threading.Thread):
    """Answers stream_mono and then writes 16 kHz silence until stopped."""

    def __init__(self, path):
        super().__init__(daemon=True)
        self.path = path
        self.stop = threading.Event()
        self.server = socket.socket(socket.AF_UNIX)
        self.server.bind(path)
        self.server.listen(4)
        self.server.settimeout(.2)

    def run(self):
        clients = []
        while not self.stop.is_set():
            try:
                client, _ = self.server.accept()
                client.recv(4096)
                client.sendall(b'{"v":1,"id":1,"ok":true,"data":{"streaming":true,"format":"pcm_s16_le","calibration_applied":true},"error":null}\n')
                clients.append(client)
            except socket.timeout:
                pass
            for client in list(clients):
                try:
                    client.sendall(b'\0' * 640)
                except OSError:
                    clients.remove(client)
            time.sleep(.02)
        for client in clients:
            client.close()
        self.server.close()


def call(path, cmd, timeout=3.0):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(timeout)
    s.connect(path)
    s.sendall(json.dumps({'v': 1, 'id': 1, 'cmd': cmd, 'args': {}}).encode() + b'\n')
    line = b''
    while not line.endswith(b'\n'):
        chunk = s.recv(4096)
        if not chunk:
            break
        line += chunk
    return s, json.loads(line)


class SubscriberSlots(unittest.TestCase):
    def setUp(self):
        if not os.access(WAKED, os.X_OK):
            if os.environ.get('WAKED_REQUIRED') == '1':
                self.fail('libreecho-waked not built: ' + WAKED)
            self.skipTest('libreecho-waked not built')
        self.dir = tempfile.mkdtemp(prefix='waked-slots-', dir=os.environ.get('TMPDIR'))
        self.mic = FakeMic(os.path.join(self.dir, 'mic.sock'))
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
        if getattr(self, 'proc', None) and self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(5)
        if getattr(self, 'mic', None):
            self.mic.stop.set()
            self.mic.join(2)

    def _fill_and_abandon(self, cmd, slots):
        for _ in range(slots):
            s, reply = call(self.sock, cmd)
            self.assertTrue(reply['ok'], reply)
            s.close()
        # No wake event and (for events) nothing to send: only noticing the
        # hang-up can free these slots.
        time.sleep(1.5)

    def test_closed_event_subscribers_release_their_slots(self):
        self._fill_and_abandon('subscribe', 4)
        s, reply = call(self.sock, 'subscribe')
        s.close()
        self.assertTrue(reply['ok'], 'a slot freed by a hung-up subscriber was not reused: %r' % reply)

    def test_closed_audio_subscribers_release_their_slots(self):
        self._fill_and_abandon('stream_audio', 2)
        s, reply = call(self.sock, 'stream_audio')
        s.close()
        self.assertTrue(reply['ok'], 'an audio slot freed by a hung-up subscriber was not reused: %r' % reply)

    def test_live_subscribers_still_hold_the_limit(self):
        held = []
        for _ in range(4):
            s, reply = call(self.sock, 'subscribe')
            self.assertTrue(reply['ok'], reply)
            held.append(s)
        time.sleep(1.5)
        s, reply = call(self.sock, 'subscribe')
        s.close()
        for h in held:
            h.close()
        self.assertFalse(reply['ok'], 'a fifth live subscriber must still be refused')


if __name__ == '__main__':
    unittest.main()
