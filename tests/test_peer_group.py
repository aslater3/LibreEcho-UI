"""Group control (#448) through real libreecho-web daemons on loopback.

Daemon a owns a group and sends commands; b is paired with a (same house
key), c is a device of another house, and a closed port stands for a device
that is off. a also lists itself, which is applied directly without a request
to its own loop. Members are reached over 127.0.0.1, so a member's microphone
state is read back from its own API.
"""
import json
import os
import sys
import tempfile
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_peer_pairing import Daemon, Responder, free_port  # noqa: E402

PAIRING = '/api/v1/network/pairing'
GROUPS = '/api/v1/network/groups'


class PeerGroups(unittest.TestCase):
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
        req = urllib.request.Request(f'http://127.0.0.1:{daemon.port}/api/v1/peer/command', data=body, method='POST')
        req.add_header('Content-Type', 'text/plain; charset=utf-8')
        try:
            with urllib.request.urlopen(req, timeout=5) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            with error:
                return error.code, json.loads(error.read() or b'null')

    def muted(self, daemon):
        return daemon.ok('GET', '/api/v1/audio')['microphone_muted']

    def test_command_reaches_paired_members_only(self):
        a = Daemon(self, 'a', self.responder.port)
        b = Daemon(self, 'b', self.responder.port)
        c = Daemon(self, 'c', self.responder.port)
        code = a.ok('POST', PAIRING, {'action': 'create'})['code']
        b.ok('POST', PAIRING, {'action': 'join', 'code': code})
        c.ok('POST', PAIRING, {'action': 'create'})

        gid = a.ok('POST', GROUPS, {'name': 'Downstairs'})['groups'][-1]['id']
        a.ok('POST', f'{GROUPS}/{gid}/members', {'address': '127.0.0.1', 'port': b.port, 'name': 'Kitchen'})
        a.ok('POST', f'{GROUPS}/{gid}/members', {'address': '127.0.0.1', 'port': c.port, 'name': 'Hall'})
        a.ok('POST', f'{GROUPS}/{gid}/members', {'address': '127.0.0.1', 'port': a.port, 'name': 'Study'})
        a.ok('POST', f'{GROUPS}/{gid}/members', {'address': '127.0.0.1', 'port': free_port(), 'name': 'Garage'})

        sent = a.ok('POST', f'{GROUPS}/{gid}/command', {'action': 'mute'})
        results = {m['name']: m['result'] for m in sent['members']}
        self.assertEqual(results, {'Kitchen': 'applied', 'Hall': 'unpaired', 'Study': 'applied', 'Garage': 'unreachable'})
        self.assertEqual((sent['applied'], sent['total'], sent['action']), (2, 4, 'mute'))
        self.assertTrue(self.muted(b))
        self.assertTrue(self.muted(a))
        self.assertFalse(self.muted(c))

        a.ok('POST', f'{GROUPS}/{gid}/command', {'action': 'unmute'})
        self.assertFalse(self.muted(b))
        self.assertFalse(self.muted(a))

        stop = a.ok('POST', f'{GROUPS}/{gid}/command', {'action': 'stop'})
        self.assertEqual(stop['applied'], 2)

    def test_member_refuses_unsigned_and_unpaired_commands(self):
        b = Daemon(self, 'b', self.responder.port)
        d = Daemon(self, 'd', self.responder.port)
        code = b.ok('POST', PAIRING, {'action': 'create'})['code']
        status, body = self.raw_post(b, b'LESIG1 00000000 group-command 0 ' + b'0' * 32 + b' ' + b'0' * 64 + b'\nmute')
        self.assertEqual((status, body['error']['code']), (401, 'not_verified'))
        status, body = self.raw_post(d, b'anything')
        self.assertEqual((status, body['error']['code']), (409, 'not_paired'))
        self.assertFalse(self.muted(b))
        d.ok('POST', PAIRING, {'action': 'join', 'code': code})
        status, body = self.raw_post(d, b'LESIG1 00000000 group-command 0 ' + b'0' * 32 + b' ' + b'0' * 64 + b'\nmute')
        self.assertEqual(status, 401, body)

    def test_groups_validate_persist_and_delete(self):
        a = Daemon(self, 'a', self.responder.port)
        a.ok('POST', PAIRING, {'action': 'create'})
        gid = a.ok('POST', GROUPS, {'name': 'Hall'})['groups'][-1]['id']
        for bad in ({'address': '999.1.1.1', 'port': 80, 'name': 'x'}, {'address': '10.0.0.2', 'port': 0, 'name': 'x'},
                    {'address': '10.0.0.2', 'port': 80}):
            status, body = a.request('POST', f'{GROUPS}/{gid}/members', bad)
            self.assertEqual(status, 400, (bad, body))
        status, body = a.request('POST', f'{GROUPS}/{gid}/command', {'action': 'ring'})
        self.assertEqual(status, 400, body)
        status, body = a.request('POST', f'{GROUPS}/999/command', {'action': 'mute'})
        self.assertEqual(status, 404, body)
        status, body = a.request('POST', GROUPS, {'name': ''})
        self.assertEqual(status, 400, body)
        status, body = a.request('PUT', GROUPS, {'name': 'x'})
        self.assertEqual(status, 405, body)
        a.ok('POST', f'{GROUPS}/{gid}/members', {'address': '10.0.0.2', 'port': 8080, 'name': 'Kitchen'})
        status, body = a.request('POST', f'{GROUPS}/{gid}/members', {'address': '10.0.0.2', 'port': 8080, 'name': 'Again'})
        self.assertEqual(status, 400, body)

        a.restart()
        listed = a.ok('GET', GROUPS)['groups']
        self.assertEqual([(g['name'], [m['name'] for m in g['members']]) for g in listed], [('Hall', ['Kitchen'])])
        self.assertEqual(a.ok('POST', f'{GROUPS}/{gid}/command', {'action': 'mute'})['members'][0]['result'], 'unreachable')

        a.ok('DELETE', f'{GROUPS}/{gid}')
        self.assertEqual(a.ok('GET', GROUPS)['groups'], [])
        status, body = a.request('POST', f'{GROUPS}/{gid}/command', {'action': 'mute'})
        self.assertEqual(status, 404, body)


if __name__ == '__main__':
    unittest.main()
