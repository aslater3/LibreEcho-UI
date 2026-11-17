"""Control-centre lease (#413) on the shared mDNS supervisor: real mdnsd over a
real seqpacket socket and an isolated services directory; no discovery daemons.

Only the fixed web executable may publish _libreecho._tcp; the record carries
TXT version, room and path=/, is withdrawn when the owner goes, coexists with
the Sendspin lease and is swept as stale at cold start.
"""
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def web_packet(port=8080, name='kitchen-echo', version='0.14.6', room='Café & <Hall>', raw=None):
    if raw is not None:
        return raw
    return (f'LIBREECHO/1 {port} name={name.encode().hex()} version={version.encode().hex()} '
            f'room={room.encode().hex()}\n').encode()


def wait_for(test, condition):
    end = time.monotonic() + 4
    while not condition() and time.monotonic() < end:
        time.sleep(.02)
    test.assertTrue(condition())


class LibreEchoLease(unittest.TestCase):
    def start(self, web_owner, sendspin_owner='/nonexistent/sendspind'):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.services = self.root / 'etc/avahi/services'
        self.services.mkdir(parents=True)
        (self.root / 'run').mkdir()
        self.airplay = self.services / 'airplay.service'
        self.airplay.write_text('AirPlay fixture')
        for name in ['libreecho-4.service', 'libreecho-5.tmp']:
            (self.services / name).write_text('stale')
        fake = self.root / 'chroot'
        fake.write_text('#!/bin/sh\ntrap "exit 0" TERM INT\ntrap : HUP\nwhile :; do sleep .05; done\n')
        fake.chmod(0o755)
        self.path = self.root / 'mdns.sock'
        binary = self.root / 'mdnsd'
        subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                        f'-DMDNS_CHROOT="{fake}"', '-DMDNS_OWNER_EXE="/nonexistent/esphomed"',
                        f'-DMDNS_SENDSPIN_OWNER_EXE="{sendspin_owner}"',
                        f'-DMDNS_WEB_OWNER_EXE="{web_owner}"',
                        str(ROOT / 'src/adapter/mdnsd.c'), str(ROOT / 'src/adapter/mdns_lease.c'),
                        '-o', str(binary)], check=True, capture_output=True, timeout=60)
        self.proc = subprocess.Popen([str(binary), '--root', str(self.root), '--socket', str(self.path)])
        self.addCleanup(self.stop)
        wait_for(self, self.path.exists)

    def stop(self):
        self.proc.terminate()
        self.proc.wait(timeout=4)

    def connect(self, data):
        owner = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        owner.settimeout(3)
        owner.connect(str(self.path))
        owner.sendall(data)
        return owner

    def records(self, kind='libreecho'):
        return sorted(self.services.glob(f'{kind}-*.service'))

    def test_publish_render_single_owner_and_withdraw(self):
        self.start(os.path.realpath(sys.executable))
        self.assertEqual(sorted(p.name for p in self.services.iterdir()), ['airplay.service'])
        with self.connect(web_packet()) as owner:
            self.assertEqual(owner.recv(64), b'pending\n')
            self.assertEqual(len(self.records()), 1)
            parsed = ET.parse(self.records()[0]).getroot()
            self.assertEqual(parsed.findtext('name'), 'kitchen-echo')
            self.assertEqual(parsed.findtext('service/type'), '_libreecho._tcp')
            self.assertEqual(parsed.findtext('service/port'), '8080')
            txt = [x.text for x in parsed.findall('service/txt-record')]
            self.assertEqual(txt, ['version=0.14.6', 'room=Café & <Hall>', 'path=/'])
            with self.connect(web_packet(port=9000)) as second:
                self.assertEqual(second.recv(64), b'error\n')
            owner.sendall(web_packet(name='hall-echo', room=''))
            self.assertEqual(owner.recv(64), b'pending\n')
            self.assertEqual(len(self.records()), 1)
            parsed = ET.parse(self.records()[0]).getroot()
            self.assertEqual(parsed.findtext('name'), 'hall-echo')
        wait_for(self, lambda: not self.records())
        self.assertTrue(self.airplay.exists())

    def test_other_executable_cannot_advertise(self):
        self.start('/usr/local/sbin/libreecho-web')
        with self.connect(web_packet()) as owner:
            self.assertEqual(owner.recv(64), b'error\n')
        self.assertEqual(self.records(), [])

    def test_malformed_registrations_rejected(self):
        self.start(os.path.realpath(sys.executable))
        bad = [
            web_packet(port=0),
            web_packet(port=65536),
            web_packet(name=''),
            web_packet(name='x' * 64),
            web_packet(version=''),
            web_packet(version='0.14"6'),
            web_packet(room='a\nb'),
            web_packet(raw=b'LIBREECHO/1 8080 name=6b version=31\n'),
            web_packet(raw=b'LIBREECHO/1 8080 name=6b version=31 room= path=2f\n'),
            web_packet(raw=b'LIBREECHO/1 8080 name=zz version=31 room=\n'),
            web_packet(raw=b'LIBREECHO/1 8080 name=6b version=31 room='),
        ]
        for data in bad:
            with self.subTest(data=data), self.connect(data) as owner:
                self.assertEqual(owner.recv(64), b'error\n')
        self.assertEqual(self.records(), [])

    def test_coexists_with_sendspin_lease(self):
        copy = Path(tempfile.mkdtemp()) / 'sendspin-owner-python'
        self.addCleanup(shutil.rmtree, copy.parent)
        shutil.copy2(os.path.realpath(sys.executable), copy)
        self.start(os.path.realpath(sys.executable), sendspin_owner=str(copy))
        helper = ('import socket,sys\n'
                  's=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET);s.connect(sys.argv[1])\n'
                  's.sendall(bytes.fromhex(sys.argv[2]));print(s.recv(64).decode().strip(),flush=True)\n'
                  'sys.stdin.read()\n')
        sendspin = subprocess.Popen([str(copy), '-c', helper, str(self.path),
                                     b'SENDSPIN/1 8928 name=4b\n'.hex()],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(sendspin.stdout.readline().strip(), 'pending')
            with self.connect(web_packet()) as owner:
                self.assertEqual(owner.recv(64), b'pending\n')
                self.assertEqual(len(self.records('sendspin')), 1)
                self.assertEqual(len(self.records('libreecho')), 1)
            wait_for(self, lambda: not self.records('libreecho'))
            self.assertEqual(len(self.records('sendspin')), 1)
        finally:
            sendspin.stdin.close()
            sendspin.wait(timeout=5)
            sendspin.stdout.close()
        wait_for(self, lambda: not self.records('sendspin'))


if __name__ == '__main__':
    unittest.main()
