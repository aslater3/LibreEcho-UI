#!/usr/bin/env python3
"""Only the nearest device answers (#408), end to end through two real waked
daemons.

Each daemon gets its own fake microphone (one loud, one quiet), its own wake
socket and a loopback UDP pair standing in for the LAN. Both are woken with
the "test" command at the same moment, and only the louder one may pass the
wake on to its subscriber. With the owner switch off, with no pairing key, or
with two different house keys, both answer as before.
"""
import json
import math
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
WAKED = os.environ.get('WAKED_BIN', str(ROOT / 'build' / 'libreecho-waked'))
CODE = '0011-2233-4455-6677-8899-aabb-ccdd-eeff-0123-4567-89ab-cdef-fedc-ba98-7654-3210'
OTHER = 'ffee-2233-4455-6677-8899-aabb-ccdd-eeff-0123-4567-89ab-cdef-fedc-ba98-7654-3210'


class FakeMic(threading.Thread):
    """Answers stream_mono, then streams 16 kHz audio at a fixed amplitude."""

    def __init__(self, path, amplitude):
        super().__init__(daemon=True)
        self.path = path
        self.stop = threading.Event()
        frame = [int(amplitude * math.sin(2 * math.pi * 440 * i / 16000)) for i in range(320)]
        self.chunk = struct.pack('<320h', *frame)
        self.server = socket.socket(socket.AF_UNIX)
        self.server.bind(path)
        self.server.listen(4)
        self.server.settimeout(.02)

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
                    client.sendall(self.chunk)
                except OSError:
                    clients.remove(client)
            time.sleep(.02)
        for client in clients:
            client.close()
        self.server.close()


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


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


def events(sock, seconds):
    """wake_detected events seen on a subscription within the time."""
    found = []
    sock.settimeout(.05)
    buf = b''
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            continue
    for line in buf.split(b'\n'):
        if b'"wake_detected"' in line:
            found.append(json.loads(line))
    return found


class Device:
    def __init__(self, root, name, amplitude, device_id, config, key, bind_port, dest_port):
        self.dir = os.path.join(root, name)
        os.mkdir(self.dir)
        self.mic = FakeMic(os.path.join(self.dir, 'mic.sock'), amplitude)
        self.mic.start()
        self.sock = os.path.join(self.dir, 'wake.sock')
        env = dict(os.environ)
        env.update({
            'LE_WAKE_ARB_CONFIG': config,
            'LE_WAKE_ARB_KEY': key,
            'LE_WAKE_ARB_BIND_PORT': str(bind_port),
            'LE_WAKE_ARB_DEST': '127.0.0.1:%d' % dest_port,
            'LE_WAKE_ARB_ID': device_id,
        })
        self.proc = subprocess.Popen(
            [WAKED, '--foreground', '--socket', self.sock,
             '--mic-socket', self.mic.path,
             '--led-socket', os.path.join(self.dir, 'led.sock'),
             '--reference-socket', os.path.join(self.dir, 'ref.sock')],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
        deadline = time.time() + 5
        while not os.path.exists(self.sock):
            if self.proc.poll() is not None or time.time() > deadline:
                err = self.proc.stderr.read() if self.proc.stderr else b''
                raise AssertionError('waked did not start: %s' % err.decode(errors='replace'))
            time.sleep(.05)
        # Let the preroll fill with this microphone's audio.
        time.sleep(1.0)
        self.sub, reply = call(self.sock, 'subscribe')
        assert reply['ok'], reply

    def status(self):
        s, reply = call(self.sock, 'status')
        s.close()
        return reply['data']

    def close(self):
        self.sub.close()
        if self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(5)
        self.mic.stop.set()
        self.mic.join(2)


class NearestAnswers(unittest.TestCase):
    def setUp(self):
        if not os.access(WAKED, os.X_OK):
            if os.environ.get('WAKED_REQUIRED') == '1':
                self.fail('libreecho-waked not built: ' + WAKED)
            self.skipTest('libreecho-waked not built')
        self.root = tempfile.mkdtemp(prefix='waked-nearest-', dir=os.environ.get('TMPDIR'))
        self.devices = []

    def tearDown(self):
        for d in self.devices:
            d.close()
            if d.proc.stderr:
                d.proc.stderr.close()
        shutil.rmtree(self.root, ignore_errors=True)

    def write(self, name, text):
        path = os.path.join(self.root, name)
        with open(path, 'w') as f:
            f.write(text)
        os.chmod(path, 0o600)
        return path

    def pair(self, switch=True, key_a=CODE, key_b=CODE, loud='a'):
        config = self.write('web-config.json', json.dumps({'hostname': 'x', 'nearest_answers': switch}))
        ka = self.write('a.key', key_a + '\n') if key_a else os.path.join(self.root, 'missing.key')
        kb = self.write('b.key', key_b + '\n') if key_b else os.path.join(self.root, 'missing.key')
        pa, pb = free_udp_port(), free_udp_port()
        # Device a has the higher id, so only loudness can make it win.
        a = Device(self.root, 'a', 12000 if loud == 'a' else 600, 'ffffffffffffffff', config, ka, pa, pb)
        self.devices.append(a)
        b = Device(self.root, 'b', 12000 if loud == 'b' else 600, '0000000000000000', config, kb, pb, pa)
        self.devices.append(b)
        return a, b

    def wake_both(self, a, b):
        sa, ra = call(a.sock, 'test')
        sb, rb = call(b.sock, 'test')
        sa.close()
        sb.close()
        self.assertTrue(ra['ok'] and rb['ok'], (ra, rb))
        got_a, got_b = [], []
        ta = threading.Thread(target=lambda: got_a.extend(events(a.sub, 1.5)))
        tb = threading.Thread(target=lambda: got_b.extend(events(b.sub, 1.5)))
        ta.start(); tb.start(); ta.join(); tb.join()
        return got_a, got_b

    def test_only_the_louder_device_answers(self):
        a, b = self.pair(loud='a')
        got_a, got_b = self.wake_both(a, b)
        self.assertEqual(len(got_a), 1, 'the louder device must answer')
        self.assertEqual(got_b, [], 'the quieter device must stay quiet')
        sa, sb = a.status()['nearest_answers'], b.status()['nearest_answers']
        self.assertTrue(sa['active'] and sb['active'], (sa, sb))
        self.assertEqual((sa['won'], sb['yielded']), (1, 1), (sa, sb))
        self.assertEqual((a.status()['detected_count'], b.status()['detected_count']), (1, 0))

    def test_louder_device_wins_whichever_it_is(self):
        a, b = self.pair(loud='b')
        got_a, got_b = self.wake_both(a, b)
        self.assertEqual((len(got_a), len(got_b)), (0, 1))

    def test_switch_off_both_answer(self):
        a, b = self.pair(switch=False)
        got_a, got_b = self.wake_both(a, b)
        self.assertEqual((len(got_a), len(got_b)), (1, 1))
        self.assertFalse(a.status()['nearest_answers']['enabled'])

    def test_unpaired_both_answer(self):
        a, b = self.pair(key_a=None, key_b=None)
        got_a, got_b = self.wake_both(a, b)
        self.assertEqual((len(got_a), len(got_b)), (1, 1))
        st = a.status()['nearest_answers']
        self.assertTrue(st['enabled'] and not st['paired'] and not st['active'], st)

    def test_other_house_key_cannot_silence(self):
        a, b = self.pair(key_b=OTHER, loud='b')
        got_a, got_b = self.wake_both(a, b)
        self.assertEqual((len(got_a), len(got_b)), (1, 1), 'claims under another key must be refused')
        self.assertGreaterEqual(a.status()['nearest_answers']['rejected'], 1)


if __name__ == '__main__':
    unittest.main()
