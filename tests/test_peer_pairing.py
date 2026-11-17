"""Device pairing and signed peer proofs (#450) through two real HTTP daemons.

Two libreecho-web instances (mock backend) run on loopback. A stand-in
legacy-unicast mDNS responder lists both, plus a forger that answers the
proof request with a bad signature, a relay that forwards the proof request
to the real second device, and a device of another house. The device list on
the first daemon marks only the genuinely paired device as "paired".
"""
import http.server
import json
from pathlib import Path
import os
import socket
import stat
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
OTHER_HOUSE = '00112233-44556677-8899aabb-ccddeeff-01234567-89abcdef-fedcba98-76543210'


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def label(text):
    data = text.encode()
    return bytes([len(data)]) + data


def rr(name, rtype, data, ttl=120):
    return name + struct.pack('!HHIH', rtype, 0x8001, ttl, len(data)) + data


class Responder(threading.Thread):
    """Answers every browse with one record per (name, port) on 127.0.0.1."""
    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.devices = []
        self.running = True

    def answers(self, query):
        # One datagram per device keeps each under the 1500-byte packet bound.
        for name, port in self.devices:
            instance = label(name) + SERVICE
            host = label(name.replace(' ', '-')) + b'\x05local\x00'
            records = [rr(SERVICE, 12, instance), rr(instance, 33, struct.pack('!HHH', 0, 0, port) + host),
                       rr(instance, 16, label('version=0.14.6')), rr(host, 1, socket.inet_aton('127.0.0.1'))]
            yield query[:2] + struct.pack('!HHHHH', 0x8400, 0, len(records), 0, 0) + b''.join(records)

    def run(self):
        while self.running:
            try:
                data, peer = self.sock.recvfrom(1500)
            except socket.timeout:
                continue
            except OSError:
                return
            for packet in self.answers(data):
                self.sock.sendto(packet, peer)


class FakePeer(threading.Thread):
    """A machine on the LAN that answers /api/v1/peer/proof itself."""
    def __init__(self, handler):
        super().__init__(daemon=True)
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                status, body = handler(self.path, self.headers.get('Host', ''))
                data = body.encode()
                self.send_response(status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.send_header('Connection', 'close')
                self.end_headers()
                self.wfile.write(data)
                outer.hits += 1

            def log_message(self, *args):
                pass

        self.hits = 0
        self.server = http.server.HTTPServer(('127.0.0.1', 0), Handler)
        self.server.timeout = 0.2
        self.port = self.server.server_address[1]
        self.running = True

    def run(self):
        while self.running:
            self.server.handle_request()

    def stop(self):
        self.running = False
        self.join(timeout=2)
        self.server.server_close()


class Daemon:
    def __init__(self, test, name, responder_port):
        self.test = test
        self.dir = Path(test.temp.name) / name
        self.dir.mkdir()
        self.config = self.dir / 'config.json'
        self.key = self.dir / 'peer.key'
        self.port = free_port()
        self.responder_port = responder_port
        self.launch()

    def launch(self):
        test = self.test
        env = dict(os.environ, LIBREECHO_MDNS_QUERY_ADDR=f'127.0.0.1:{self.responder_port}',
                   LIBREECHO_MDNS_SOCKET=str(self.dir / 'no-mdns.sock'))
        log = open(self.dir / 'server.log', 'ab')
        test.addCleanup(log.close)
        proc = subprocess.Popen([str(ROOT / 'build/libreecho-web'), '--backend', 'mock', '--config', str(self.config),
                                 '--mock-config', str(ROOT / 'config/mock-state.json'), '--web-root', str(ROOT / 'web'),
                                 '--listen', f'0.0.0.0:{self.port}', '--seed', '42', '--allow-insecure-lan'],
                                env=env, stdout=log, stderr=log)
        self.proc = proc

        def stop():
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=10)
        test.addCleanup(stop)
        end = time.monotonic() + 12
        while time.monotonic() < end:
            try:
                self.csrf = self.request('GET', '/api/v1/config')[1]['data']['csrf_token']
                return
            except (OSError, ValueError, TypeError):
                time.sleep(0.1)
        raise AssertionError((self.dir / 'server.log').read_text(errors='replace'))

    def restart(self):
        self.proc.terminate()
        self.proc.wait(timeout=10)
        self.launch()

    def request(self, method, path, body=None, csrf=True, host=None):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(f'http://127.0.0.1:{self.port}{path}', data=data, method=method)
        if body is not None:
            req.add_header('Content-Type', 'application/json')
        if csrf and method not in ('GET', 'HEAD'):
            req.add_header('X-LibreEcho-CSRF', self.csrf)
        if host:
            req.add_header('Host', host)
        try:
            with urllib.request.urlopen(req, timeout=10) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            with error:
                return error.code, json.loads(error.read() or b'null')

    def ok(self, method, path, body=None):
        status, reply = self.request(method, path, body)
        self.test.assertEqual(status, 200, reply)
        return reply['data']


NONCE = '0123456789abcdef0123456789abcdef'


class PeerPairing(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        self.addCleanup(self.temp.cleanup)
        self.responder = Responder()
        self.responder.start()
        self.addCleanup(self.stop_responder)

    def stop_responder(self):
        self.responder.running = False
        self.responder.join(timeout=2)
        self.responder.sock.close()

    def fake(self, handler):
        peer = FakePeer(handler)
        peer.start()
        self.addCleanup(peer.stop)
        return peer

    def test_pairing_api(self):
        a = Daemon(self, 'a', self.responder.port)
        self.assertEqual(a.ok('GET', '/api/v1/network/pairing'), {'supported': True, 'paired': False, 'key_id': ''})
        status, body = a.request('GET', f'/api/v1/peer/proof?nonce={NONCE}', host=f'127.0.0.1:{a.port}')
        self.assertEqual((status, body['error']['code']), (409, 'not_paired'))
        status, body = a.request('POST', '/api/v1/network/pairing', {'action': 'show'})
        self.assertEqual(status, 409, body)
        # Changing requests need the CSRF token like every other mutation.
        status, body = a.request('POST', '/api/v1/network/pairing', {'action': 'create'}, csrf=False)
        self.assertEqual(status, 403, body)
        self.assertFalse(a.key.exists())
        for bad in ({}, {'action': 'make'}, {'action': 'create', 'code': OTHER_HOUSE}, {'action': 'join'},
                    {'action': 'join', 'code': 'abcd'}, {'action': 'join', 'code': OTHER_HOUSE + '0'},
                    {'action': 'create', 'extra': 1}, {'action': 7}):
            status, body = a.request('POST', '/api/v1/network/pairing', bad)
            self.assertEqual(status, 400, (bad, body))
        self.assertFalse(a.key.exists())
        status, body = a.request('PUT', '/api/v1/network/pairing', {'action': 'create'})
        self.assertEqual(status, 405, body)
        status, body = a.request('DELETE', f'/api/v1/peer/proof?nonce={NONCE}')
        self.assertEqual(status, 405, body)

        created = a.ok('POST', '/api/v1/network/pairing', {'action': 'create'})
        self.assertTrue(created['paired'])
        self.assertRegex(created['code'], r'^[0-9a-f]{4}(-[0-9a-f]{4}){15}$')
        self.assertRegex(created['key_id'], r'^[0-9a-f]{8}$')
        self.assertEqual(stat.S_IMODE(a.key.stat().st_mode), 0o600)
        self.assertEqual(a.key.read_text().strip(), created['code'])
        self.assertFalse(Path(str(a.key) + '.bak').exists())
        # The key is not in the saved configuration or any GET answer.
        self.assertNotIn(created['code'].replace('-', '')[:16], self.config_text(a))
        self.assertEqual(a.ok('GET', '/api/v1/network/pairing'), {'supported': True, 'paired': True, 'key_id': created['key_id']})
        self.assertNotIn('code', a.ok('GET', '/api/v1/network/peers'))
        self.assertEqual(a.ok('POST', '/api/v1/network/pairing', {'action': 'show'})['code'], created['code'])
        status, body = a.request('POST', '/api/v1/network/pairing', {'action': 'create'})
        self.assertEqual(status, 409, body)
        status, body = a.request('POST', '/api/v1/network/pairing', {'action': 'join', 'code': OTHER_HOUSE})
        self.assertEqual(status, 409, body)

        # The proof endpoint: needs a well-formed nonce, the device's own
        # address and port, and GET.
        status, body = a.request('GET', '/api/v1/peer/proof?nonce=abc', host=f'127.0.0.1:{a.port}')
        self.assertEqual(status, 400, body)
        status, body = a.request('GET', f'/api/v1/peer/proof?nonce={NONCE.upper()}', host=f'127.0.0.1:{a.port}')
        self.assertEqual(status, 400, body)
        status, body = a.request('GET', f'/api/v1/peer/proof?nonce={NONCE}', host=f'127.0.0.1:{a.port + 1}')
        self.assertEqual(status, 400, body)
        status, body = a.request('GET', f'/api/v1/peer/proof?nonce={NONCE}', host=f'192.0.2.77:{a.port}')
        self.assertEqual(status, 400, body)
        status, body = a.request('GET', f'/api/v1/peer/proof?nonce={NONCE}', host='attacker.example')
        self.assertEqual(status, 400, body)
        status, body = a.request('POST', f'/api/v1/peer/proof?nonce={NONCE}', {}, csrf=False)
        self.assertEqual(status, 403, body)
        status, body = a.request('GET', f'/api/v1/peer/proof?nonce={NONCE}', host=f'127.0.0.1:{a.port}')
        self.assertEqual(status, 200, body)
        proof = body['data']
        self.assertEqual((proof['key_id'], proof['type'], proof['nonce'], proof['host']),
                         (created['key_id'], 'peer-proof', NONCE, f'127.0.0.1:{a.port}'))
        self.assertRegex(proof['mac'], r'^[0-9a-f]{64}$')

        # The key survives a restart, and DELETE unpairs and removes it.
        a.restart()
        self.assertEqual(a.ok('GET', '/api/v1/network/pairing')['key_id'], created['key_id'])
        b = Daemon(self, 'b', self.responder.port)
        status, body = b.request('POST', '/api/v1/network/pairing', {'action': 'join', 'code': created['code'].upper()})
        self.assertEqual(status, 200, body)
        self.assertEqual(body['data']['key_id'], created['key_id'])
        self.assertNotIn('code', body['data'])
        self.assertEqual(a.ok('DELETE', '/api/v1/network/pairing'), {'supported': True, 'paired': False, 'key_id': ''})
        self.assertFalse(a.key.exists())
        status, body = a.request('GET', f'/api/v1/peer/proof?nonce={NONCE}', host=f'127.0.0.1:{a.port}')
        self.assertEqual(status, 409, body)

    def config_text(self, daemon):
        return daemon.config.read_text() if daemon.config.exists() else ''

    def test_device_list_marks_only_paired_devices(self):
        a = Daemon(self, 'a', self.responder.port)
        b = Daemon(self, 'b', self.responder.port)
        stranger = Daemon(self, 'stranger', self.responder.port)
        unpaired = Daemon(self, 'unpaired', self.responder.port)

        def forge(path, host):
            nonce = path.split('nonce=', 1)[-1]
            return 200, json.dumps({'ok': True, 'data': {
                'key_id': self.key_id, 'type': 'peer-proof', 'ts': int(time.time()), 'nonce': nonce,
                'name': 'forger', 'room': '', 'version': '0.14.6', 'host': host, 'mac': 'ab' * 32}, 'error': None})
        forger = self.fake(forge)

        def relay(path, host):
            # Forwards to the real paired device; it refuses a foreign Host,
            # and a rewritten Host is caught by the asking device.
            req = urllib.request.Request(f'http://127.0.0.1:{b.port}{path}', headers={'Host': host})
            try:
                with urllib.request.urlopen(req, timeout=2) as response:
                    first = response.read().decode()
            except urllib.error.HTTPError as error:
                first = error.read().decode()
            self.relay_first = first
            req = urllib.request.Request(f'http://127.0.0.1:{b.port}{path}', headers={'Host': f'127.0.0.1:{b.port}'})
            try:
                with urllib.request.urlopen(req, timeout=2) as response:
                    return 200, response.read().decode()
            except urllib.error.HTTPError as error:
                with error:
                    return error.code, error.read().decode()
        relayer = self.fake(relay)
        silent_port = free_port()

        self.responder.devices = [('libreecho-dev', a.port), ('kitchen', b.port), ('forger', forger.port),
                                  ('relay', relayer.port), ('neighbour', stranger.port), ('new-echo', unpaired.port),
                                  ('gone', silent_port)]
        # Before pairing nothing is checked or sent.
        data = a.ok('GET', '/api/v1/network/peers')
        self.assertFalse(data['paired'])
        self.assertEqual({d['trust'] for d in data['devices']}, {'off'}, data)
        self.assertEqual((forger.hits, relayer.hits), (0, 0))

        code = a.ok('POST', '/api/v1/network/pairing', {'action': 'create'})['code']
        self.key_id = a.ok('GET', '/api/v1/network/pairing')['key_id']
        b.ok('POST', '/api/v1/network/pairing', {'action': 'join', 'code': code})
        stranger.ok('POST', '/api/v1/network/pairing', {'action': 'join', 'code': OTHER_HOUSE})
        time.sleep(3.1)  # past the device-list cache
        started = time.monotonic()
        data = a.ok('GET', '/api/v1/network/peers')
        elapsed = time.monotonic() - started
        self.assertTrue(data['paired'])
        self.assertEqual(data['key_id'], self.key_id)
        trust = {d['name']: d['trust'] for d in data['devices']}
        self.assertEqual(trust, {'libreecho-dev': 'self', 'kitchen': 'paired', 'forger': 'failed', 'relay': 'failed',
                                 'neighbour': 'unpaired', 'new-echo': 'unpaired', 'gone': 'failed'})
        self.assertGreaterEqual(forger.hits, 1)
        self.assertGreaterEqual(relayer.hits, 1)
        self.assertIn('own address', self.relay_first)
        # Browse (1 s) plus the bounded parallel check (0.8 s).
        self.assertLess(elapsed, 3.5)

        # Unpairing the other device turns it to unpaired.
        b.ok('DELETE', '/api/v1/network/pairing')
        time.sleep(3.1)
        trust = {d['name']: d['trust'] for d in a.ok('GET', '/api/v1/network/peers')['devices']}
        self.assertEqual(trust['kitchen'], 'unpaired')


if __name__ == '__main__':
    unittest.main()
