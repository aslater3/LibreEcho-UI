"""Device list (#413) through the real HTTP daemon and mock backend.

GET /api/v1/network/peers browses a stand-in legacy-unicast mDNS responder on
loopback (LIBREECHO_MDNS_QUERY_ADDR) and reports what it found. The daemon's
own _libreecho._tcp lease is taken over a stand-in supervisor socket
(LIBREECHO_MDNS_SOCKET): it registers only while discoverable is on and the
listener is reachable from the LAN, re-registers when that is turned back on,
and the choice persists in the saved configuration.
"""
import json
from pathlib import Path
import os
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SERVICE = b'\x0a_libreecho\x04_tcp\x05local\x00'


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def label(text):
    data = text.encode()
    return bytes([len(data)]) + data


def rr(name, rtype, data, ttl=120):
    return name + struct.pack('!HHIH', rtype, 0x8001, ttl, len(data)) + data


def answer(query, self_port):
    qid = query[:2]
    instance = label('hall-echo') + SERVICE
    # This device's own record, renamed by Avahi after a hostname clash, and the
    # other device that kept the mock backend's hostname ("libreecho-dev").
    own = label('libreecho-dev #2') + SERVICE
    twin = label('libreecho-dev') + SERVICE
    host = label('hall-echo') + b'\x05local\x00'
    own_host = label('libreecho-dev-2') + b'\x05local\x00'
    twin_host = label('libreecho-dev') + b'\x05local\x00'
    txt = b''.join(label(x) for x in ['version=0.14.6', 'room=Hall "B" & co', 'path=/'])
    records = [
        rr(SERVICE, 12, instance), rr(SERVICE, 12, own), rr(SERVICE, 12, twin),
        rr(instance, 33, struct.pack('!HHH', 0, 0, 8080) + host), rr(instance, 16, txt),
        rr(own, 33, struct.pack('!HHH', 0, 0, self_port) + own_host), rr(own, 16, label('version=0.14.6')),
        rr(twin, 33, struct.pack('!HHH', 0, 0, self_port) + twin_host), rr(twin, 16, label('version=0.14.6')),
        rr(host, 1, socket.inet_aton('127.0.0.1')),
        rr(own_host, 1, socket.inet_aton('127.0.0.1')),
        rr(twin_host, 1, socket.inet_aton('192.0.2.30')),
    ]
    return qid + struct.pack('!HHHHH', 0x8400, 0, len(records), 0, 0) + b''.join(records)


class Responder(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.queries = 0
        self.self_port = 0
        self.running = True

    def run(self):
        while self.running:
            try:
                data, peer = self.sock.recvfrom(1500)
            except socket.timeout:
                continue
            except OSError:
                return
            self.queries += 1
            self.sock.sendto(answer(data, self.self_port), peer)


class Supervisor(threading.Thread):
    """Accepts registrations like libreecho-mdnsd and records the live one."""
    def __init__(self, path):
        super().__init__(daemon=True)
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.server.bind(path)
        self.server.listen(4)
        self.server.settimeout(0.2)
        self.messages = []
        self.live = 0
        self.running = True

    def run(self):
        while self.running:
            try:
                conn, _ = self.server.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            threading.Thread(target=self.serve, args=(conn,), daemon=True).start()

    def serve(self, conn):
        with conn:
            message = conn.recv(1536)
            self.messages.append(message)
            conn.sendall(b'pending\n')
            self.live += 1
            try:
                while conn.recv(64):
                    pass
            except OSError:
                pass
            self.live -= 1


def wait_for(condition, seconds=12):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if condition():
            return True
        time.sleep(0.1)
    return condition()


class NetworkPeers(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        self.responder = Responder()
        self.responder.start()
        self.supervisor = Supervisor(str(self.dir / 'mdns.sock'))
        self.supervisor.start()
        self.config = self.dir / 'config.json'
        self.port = free_port()
        self.responder.self_port = self.port

    def tearDown(self):
        self.responder.running = self.supervisor.running = False
        self.responder.join(timeout=2)
        self.supervisor.join(timeout=2)
        self.responder.sock.close()
        self.supervisor.server.close()

    def start(self, listen='0.0.0.0'):
        env = dict(os.environ, LIBREECHO_MDNS_QUERY_ADDR=f'127.0.0.1:{self.responder.port}',
                   LIBREECHO_MDNS_SOCKET=str(self.dir / 'mdns.sock'))
        log = open(self.dir / 'server.log', 'ab')
        self.addCleanup(log.close)
        proc = subprocess.Popen([str(ROOT / 'build/libreecho-web'), '--backend', 'mock', '--config', str(self.config),
                                 '--mock-config', str(ROOT / 'config/mock-state.json'), '--web-root', str(ROOT / 'web'),
                                 '--listen', f'{listen}:{self.port}', '--seed', '42', '--allow-insecure-lan'],
                                env=env, stdout=log, stderr=log)
        def stop():
            proc.terminate()
            proc.wait(timeout=10)
        self.addCleanup(stop)
        self.assertTrue(wait_for(lambda: self.get('/api/v1/config', check=False) is not None),
                        (self.dir / 'server.log').read_text(errors='replace'))
        self.csrf = self.get('/api/v1/config')['data']['csrf_token']

    def request(self, method, path, body=None):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(f'http://127.0.0.1:{self.port}{path}', data=data, method=method)
        if body is not None:
            req.add_header('Content-Type', 'application/json')
            req.add_header('X-LibreEcho-CSRF', self.csrf)
        try:
            with urllib.request.urlopen(req, timeout=10) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            with error:
                return error.code, json.loads(error.read() or b'null')

    def get(self, path, check=True):
        try:
            status, body = self.request('GET', path)
        except (OSError, ValueError):
            if check:
                raise
            return None
        if check:
            self.assertEqual(status, 200, body)
        return body

    def test_lists_devices_and_marks_self(self):
        self.start()
        data = self.get('/api/v1/network/peers')['data']
        self.assertTrue(data['discoverable'])
        self.assertFalse(data['truncated'])
        # Not paired (#450): nothing is checked and no key id is reported.
        self.assertEqual((data['paired'], data['key_id']), (False, ''))
        by_name = {d['name']: d for d in data['devices']}
        self.assertEqual(sorted(by_name), ['hall-echo', 'libreecho-dev', 'libreecho-dev #2'])
        hall = by_name['hall-echo']
        self.assertEqual(hall, {'name': 'hall-echo', 'room': 'Hall "B" & co', 'version': '0.14.6',
                                'address': '127.0.0.1', 'port': 8080, 'url': 'http://127.0.0.1:8080/',
                                'self': False, 'trust': 'off'})
        # Self is this device's address and HTTP port, not its hostname: the
        # renamed own record is self, the other device on the same hostname is not.
        self.assertTrue(by_name['libreecho-dev #2']['self'])
        self.assertEqual(by_name['libreecho-dev #2']['room'], '')
        self.assertFalse(by_name['libreecho-dev']['self'])
        self.assertEqual(by_name['libreecho-dev']['address'], '192.0.2.30')
        queries = self.responder.queries
        # A second request inside the cache window does not browse again.
        self.get('/api/v1/network/peers')
        self.assertEqual(self.responder.queries, queries)
        status, body = self.request('POST', '/api/v1/network/peers', {})
        self.assertEqual(status, 405, body)
        self.assertFalse(body['ok'])

    def test_advertises_only_while_discoverable_and_persists(self):
        self.start()
        self.assertTrue(wait_for(lambda: self.supervisor.live == 1))
        message = self.supervisor.messages[-1].decode()
        self.assertTrue(message.startswith(f'LIBREECHO/1 {self.port} name={b"libreecho-dev".hex()} version='),
                        message)
        self.assertTrue(message.endswith(' room=\n'))
        self.assertTrue(self.get('/api/v1/network')['data']['discoverable'])
        self.assertTrue(self.get('/api/v1/network/peers')['data']['advertising'])

        status, body = self.request('PUT', '/api/v1/network', {'discoverable': False})
        self.assertEqual(status, 200, body)
        self.assertTrue(wait_for(lambda: self.supervisor.live == 0))
        self.assertFalse(self.get('/api/v1/network')['data']['discoverable'])
        self.assertFalse(json.loads(self.config.read_text())['discoverable'])
        self.assertFalse(self.get('/api/v1/network/peers')['data']['advertising'])

        # A room change reaches the record once advertising is back on.
        status, body = self.request('PUT', '/api/v1/device', {'room': 'Hall'})
        self.assertEqual(status, 200, body)
        count = len(self.supervisor.messages)
        status, body = self.request('PUT', '/api/v1/network', {'discoverable': True})
        self.assertEqual(status, 200, body)
        self.assertTrue(wait_for(lambda: self.supervisor.live == 1 and len(self.supervisor.messages) > count))
        self.assertTrue(self.supervisor.messages[-1].decode().endswith(f' room={b"Hall".hex()}\n'))
        self.assertTrue(json.loads(self.config.read_text())['discoverable'])

    def test_loopback_listener_is_never_advertised(self):
        self.start(listen='127.0.0.1')
        time.sleep(2.5)
        self.assertEqual(self.supervisor.messages, [])
        data = self.get('/api/v1/network/peers')['data']
        self.assertTrue(data['discoverable'])
        self.assertFalse(data['advertising'])


if __name__ == '__main__':
    unittest.main()
