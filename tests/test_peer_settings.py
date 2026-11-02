"""Settings copy (#449) through real libreecho-web daemons on loopback.

Daemon a is the owner's device and copies settings to b, which is paired with
a (same house key). c is a device of another house, and a closed port stands
for a device that is off. The packets forged here are sealed with the house
key in Python, following peer_auth.h, so a mismatch with the daemon's own MAC
would show up as a refused packet.
"""
import hashlib
import hmac
import json
import os
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_peer_pairing import Daemon, Responder, free_port  # noqa: E402

PAIRING = '/api/v1/network/pairing'
COPY = '/api/v1/network/settings/copy'
EXPORT = '/api/v1/config/export'
PACKET_PATH = '/api/v1/peer/settings'
TYPE = b'settings-copy'
IDENTITY = ('hostname', 'room', 'ssh', 'api_lan', 'discoverable', 'mac_wifi', 'mac_bt', 'timezone',
            'privacy_local_only', 'privacy_telemetry', 'feature_https', 'button_short', 'button_long')


def seal(code, body, ts, nonce):
    """A sealed packet as peer_auth.h describes it, from the pairing code."""
    secret = bytes.fromhex(code.replace('-', ''))
    mac_key = hmac.new(secret, b'LibreEcho peer mac v1', hashlib.sha256).digest()
    key_id = hmac.new(secret, b'LibreEcho key id v1', hashlib.sha256).digest()[:4].hex()
    text = b'LESIG1\n' + TYPE + b'\n' + str(ts).encode() + b'\n' + nonce.encode() + b'\n' + body
    mac = hmac.new(mac_key, text, hashlib.sha256).hexdigest()
    return f'LESIG1 {key_id} {TYPE.decode()} {ts} {nonce} {mac}\n'.encode() + body


class PeerSettings(unittest.TestCase):
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

    def raw_post(self, daemon, body):
        req = urllib.request.Request(f'http://127.0.0.1:{daemon.port}{PACKET_PATH}', data=body, method='POST')
        req.add_header('Content-Type', 'text/plain; charset=utf-8')
        try:
            with urllib.request.urlopen(req, timeout=5) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            with error:
                return error.code, json.loads(error.read() or b'null')

    def export(self, daemon):
        return daemon.ok('GET', EXPORT)

    def paired_pair(self):
        a = Daemon(self, 'a', self.responder.port)
        b = Daemon(self, 'b', self.responder.port)
        code = a.ok('POST', PAIRING, {'action': 'create'})['code']
        b.ok('POST', PAIRING, {'action': 'join', 'code': code})
        return a, b, code

    def test_copy_moves_only_the_chosen_groups(self):
        a, b, _ = self.paired_pair()
        a.ok('PUT', '/api/v1/audio', {'volume': 33, 'microphone_gain': 44})
        a.ok('PUT', '/api/v1/led', {'brightness': 35, 'visualizer_enabled': False})
        a.ok('PUT', '/api/v1/wake-word', {'sensitivity': 71})
        a.ok('PUT', '/api/v1/home-assistant', {'url': 'http://ha.example:8123'})
        before = self.export(b)

        sent = a.ok('POST', COPY, {'address': '127.0.0.1', 'port': b.port, 'audio': True, 'led': True})
        self.assertEqual((sent['result'], sent['groups']), ('applied', 'audio,led'))
        after = self.export(b)
        self.assertEqual((after['volume'], after['microphone_gain']), (33, 44))
        self.assertEqual((after['led_brightness'], after['led_visualizer_enabled']), (35, False))
        self.assertEqual(after['wake_sensitivity'], before['wake_sensitivity'])
        self.assertEqual(after.get('home_assistant_url'), before.get('home_assistant_url'))
        for key in IDENTITY:
            self.assertEqual(after.get(key), before.get(key), key)

        sent = a.ok('POST', COPY, {'address': '127.0.0.1', 'port': b.port, 'voice': True, 'ha': True})
        self.assertEqual((sent['result'], sent['groups']), ('applied', 'voice,ha'))
        after = self.export(b)
        self.assertEqual(after['wake_sensitivity'], 71)
        self.assertEqual(after['home_assistant_url'], 'http://ha.example:8123')
        self.assertEqual(after['volume'], 33)
        for key in IDENTITY:
            self.assertEqual(after.get(key), before.get(key), key)

    def test_member_applies_only_listed_keys_and_refuses_bad_packets(self):
        a, b, code = self.paired_pair()
        before = self.export(b)
        ts = int(time.time())
        forged = seal(code, b'{"volume":12,"hostname":"evil-host","ssh":true,"api_lan":true}', ts, '1' * 32)
        status, body = self.raw_post(b, forged)
        self.assertEqual((status, body['data']), (200, {'applied': True}), body)
        after = self.export(b)
        self.assertEqual(after['volume'], 12)
        for key in ('hostname', 'ssh', 'api_lan'):
            self.assertEqual(after.get(key), before.get(key), key)

        status, body = self.raw_post(b, forged)
        self.assertEqual((status, body['error']['code']), (401, 'not_verified'))

        status, body = self.raw_post(b, seal(code, b'{"volume":500}', ts, '2' * 32))
        self.assertEqual(status, 400, body)
        status, body = self.raw_post(b, seal(code, b'{"led_visualizer_enabled":"yes"}', ts, '3' * 32))
        self.assertEqual(status, 400, body)
        self.assertEqual(self.export(b)['volume'], 12)

        status, body = self.raw_post(b, seal(code, b'{"volume":40}', ts - 3600, '4' * 32))
        self.assertEqual((status, body['error']['code']), (401, 'not_verified'))
        status, body = self.raw_post(b, b'LESIG1 00000000 settings-copy 0 ' + b'0' * 32 + b' ' + b'0' * 64 + b'\n{"volume":1}')
        self.assertEqual((status, body['error']['code']), (401, 'not_verified'))
        self.assertEqual(self.export(b)['volume'], 12)

    def test_unpaired_member_refuses_packets(self):
        d = Daemon(self, 'd', self.responder.port)
        status, body = self.raw_post(d, b'anything')
        self.assertEqual((status, body['error']['code']), (409, 'not_paired'))

    def test_copy_validates_the_request_and_reports_each_result(self):
        a, b, _ = self.paired_pair()
        c = Daemon(self, 'c', self.responder.port)
        c.ok('POST', PAIRING, {'action': 'create'})
        d = Daemon(self, 'd', self.responder.port)
        group = {'audio': True}
        status, body = a.request('POST', COPY, {'address': '127.0.0.1', 'port': b.port})
        self.assertEqual(status, 400, body)
        status, body = a.request('POST', COPY, {'address': '999.1.1.1', 'port': b.port, 'audio': True})
        self.assertEqual(status, 400, body)
        status, body = a.request('POST', COPY, {'address': '127.0.0.1', 'port': a.port, 'audio': True})
        self.assertEqual(status, 400, body)
        status, body = a.request('POST', COPY, {'address': '127.0.0.1', 'port': b.port, 'audio': 'yes'})
        self.assertEqual(status, 400, body)
        status, body = a.request('POST', COPY, {'address': '127.0.0.1', 'port': b.port, 'ha': True})
        self.assertEqual(status, 409, body)
        status, body = a.request('POST', COPY, group, csrf=False)
        self.assertEqual(status, 403, body)
        status, body = d.request('POST', COPY, {'address': '127.0.0.1', 'port': b.port, 'audio': True})
        self.assertEqual(status, 409, body)

        self.assertEqual(a.ok('POST', COPY, {'address': '127.0.0.1', 'port': c.port, 'audio': True})['result'], 'unpaired')
        self.assertEqual(a.ok('POST', COPY, {'address': '127.0.0.1', 'port': free_port(), 'audio': True})['result'], 'unreachable')


if __name__ == '__main__':
    unittest.main()
