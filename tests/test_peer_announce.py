"""Broadcast announcements (#412, first slice) through real libreecho-web daemons on loopback.

Daemon a is the owner and b is paired with a (same house key). c is a device of
another house, d has no key, and a closed port stands for a device that is off.
Every daemon uses the mock backend, which has no speaker: a verified
announcement is answered 501 not_supported there, so this test proves the
packet is authenticated, validated and replay-protected. It does NOT prove
that anything was spoken; that is hardware evidence and is owed separately.
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
from test_peer_pairing import Daemon, Responder, free_port, OTHER_HOUSE  # noqa: E402

PAIRING = '/api/v1/network/pairing'
BROADCAST = '/api/v1/network/broadcast'
PACKET_PATH = '/api/v1/peer/announce'
TYPE = b'announcement'


def seal(code, body, ts, nonce):
    """A sealed packet as peer_auth.h describes it, from the pairing code."""
    secret = bytes.fromhex(code.replace('-', ''))
    mac_key = hmac.new(secret, b'LibreEcho peer mac v1', hashlib.sha256).digest()
    key_id = hmac.new(secret, b'LibreEcho key id v1', hashlib.sha256).digest()[:4].hex()
    text = b'LESIG1\n' + TYPE + b'\n' + str(ts).encode() + b'\n' + nonce.encode() + b'\n' + body
    mac = hmac.new(mac_key, text, hashlib.sha256).hexdigest()
    return f'LESIG1 {key_id} {TYPE.decode()} {ts} {nonce} {mac}\n'.encode() + body


class PeerAnnounce(unittest.TestCase):
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

    def test_broadcast_reaches_paired_devices_and_skips_strangers(self):
        a = Daemon(self, 'a', self.responder.port)
        b = Daemon(self, 'b', self.responder.port)
        c = Daemon(self, 'c', self.responder.port)
        code = a.ok('POST', PAIRING, {'action': 'create'})['code']
        b.ok('POST', PAIRING, {'action': 'join', 'code': code})
        c.ok('POST', PAIRING, {'action': 'join', 'code': OTHER_HOUSE})
        self.responder.devices = [('owner', a.port), ('kitchen', b.port), ('stranger', c.port)]

        # One device: a signed packet goes to b, which verifies it and reports the mock's missing speaker.
        one = a.ok('POST', BROADCAST, {'text': 'Dinner is ready', 'address': '127.0.0.1', 'port': b.port})
        self.assertEqual((one['sent'], one['applied'], one['skipped']), (1, 0, 0), one)
        self.assertEqual(one['devices'], [{'address': '127.0.0.1', 'port': b.port, 'result': 'not_supported'}], one)

        # Whole house: b is reached, c is another house and skipped, a is itself and is not sent to.
        house = a.ok('POST', BROADCAST, {'text': 'Tea is made'})
        self.assertEqual(house['this_device'], 'not_supported', house)
        self.assertEqual([d['name'] for d in house['devices']], ['kitchen'], house)
        self.assertEqual(house['devices'][0]['result'], 'not_supported', house)
        self.assertEqual((house['sent'], house['skipped']), (1, 1), house)
        self.assertFalse(house['truncated'], house)

    def test_member_verifies_then_refuses_forged_and_replayed_packets(self):
        a = Daemon(self, 'a', self.responder.port)
        b = Daemon(self, 'b', self.responder.port)
        code = a.ok('POST', PAIRING, {'action': 'create'})['code']
        b.ok('POST', PAIRING, {'action': 'join', 'code': code})
        ts = int(time.time())

        good = seal(code, b'Dinner is ready', ts, '1' * 32)
        status, body = self.raw_post(b, good)
        self.assertEqual((status, body['data']), (501, None), body)
        self.assertEqual(body['error']['code'], 'not_supported', body)

        status, body = self.raw_post(b, good)
        self.assertEqual((status, body['error']['code']), (401, 'not_verified'), body)

        status, body = self.raw_post(b, seal(OTHER_HOUSE, b'Forged', ts, '2' * 32))
        self.assertEqual((status, body['error']['code']), (401, 'not_verified'), body)
        status, body = self.raw_post(b, seal(code, b'Old', ts - 3600, '3' * 32))
        self.assertEqual((status, body['error']['code']), (401, 'not_verified'), body)
        status, body = self.raw_post(b, b'LESIG1 00000000 announcement 0 ' + b'0' * 32 + b' ' + b'0' * 64 + b'\nHi')
        self.assertEqual((status, body['error']['code']), (401, 'not_verified'), body)

    def test_member_refuses_bad_text_with_a_verified_packet(self):
        a = Daemon(self, 'a', self.responder.port)
        b = Daemon(self, 'b', self.responder.port)
        code = a.ok('POST', PAIRING, {'action': 'create'})['code']
        b.ok('POST', PAIRING, {'action': 'join', 'code': code})
        ts = int(time.time())
        for body, nonce in ((b'', '4' * 32), (b'   ', '5' * 32), (b'line\nbreak', '6' * 32), (b'x' * 241, '7' * 32)):
            status, reply = self.raw_post(b, seal(code, body, ts, nonce))
            self.assertEqual((status, reply['error']['code']), (400, 'invalid_request'), (body, reply))

    def test_unpaired_devices_send_and_refuse(self):
        d = Daemon(self, 'd', self.responder.port)
        status, body = self.raw_post(d, b'anything')
        self.assertEqual((status, body['error']['code']), (409, 'not_paired'))
        status, body = d.request('POST', BROADCAST, {'text': 'Hello'})
        self.assertEqual(status, 409, body)

    def test_broadcast_validates_the_request(self):
        a = Daemon(self, 'a', self.responder.port)
        b = Daemon(self, 'b', self.responder.port)
        code = a.ok('POST', PAIRING, {'action': 'create'})['code']
        b.ok('POST', PAIRING, {'action': 'join', 'code': code})
        status, body = a.request('POST', BROADCAST, {'text': ''})
        self.assertEqual(status, 400, body)
        status, body = a.request('POST', BROADCAST, {'text': 'line\nbreak'})
        self.assertEqual(status, 400, body)
        status, body = a.request('POST', BROADCAST, {'text': 'x' * 241})
        self.assertEqual(status, 400, body)
        status, body = a.request('POST', BROADCAST, {'text': 'Hi', 'address': '999.1.1.1', 'port': b.port})
        self.assertEqual(status, 400, body)
        status, body = a.request('POST', BROADCAST, {'text': 'Hi', 'address': '127.0.0.1'})
        self.assertEqual(status, 400, body)
        status, body = a.request('POST', BROADCAST, {'text': 'Hi'}, csrf=False)
        self.assertEqual(status, 403, body)
        status, body = a.request('POST', BROADCAST, {'text': 'Hi', 'address': '127.0.0.1', 'port': free_port()})
        self.assertEqual(status, 200, body)
        self.assertEqual(body['data']['devices'][0]['result'], 'unreachable', body)


if __name__ == '__main__':
    unittest.main()
