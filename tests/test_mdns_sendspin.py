"""Sendspin player lease on the shared mDNS supervisor: real mdnsd over a real
seqpacket socket and an isolated services directory; no discovery daemons.

The supervisor publishes _sendspin._tcp with TXT path=/sendspin and name only
for the fixed player executable, withdraws it when the owner goes, keeps one
record per kind, and leaves the ESPHome lease and static records alone.
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
ESPHOME_FIELDS = dict(version='2026.9.0', mac='02:00:00:00:00:01', platform='LibreEcho',
                      board='radar-puffin', network='wifi', friendly_name='Kitchen',
                      api_encryption='Noise_NNpsk0_25519_ChaChaPoly_SHA256',
                      project_name='LibreEcho', project_version='0.15.0')


def sendspin_packet(port=8928, name='Kitchen café & <Echo> "one"', raw=None):
    body = raw if raw is not None else name.encode().hex()
    return f'SENDSPIN/1 {port} name={body}\n'.encode()


def esphome_packet(port=6053):
    return ('ESPHOME/1 ' + str(port) + ''.join(' ' + k + '=' + v.encode().hex()
            for k, v in ESPHOME_FIELDS.items()) + '\n').encode()


def wait_for(test, condition):
    end = time.monotonic() + 4
    while not condition() and time.monotonic() < end:
        time.sleep(.02)
    test.assertTrue(condition())


class SendspinLease(unittest.TestCase):
    def start(self, sendspin_owner, esphome_owner='/nonexistent/esphomed'):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.services = self.root / 'etc/avahi/services'
        self.services.mkdir(parents=True)
        (self.root / 'run').mkdir()
        self.airplay = self.services / 'airplay.service'
        self.airplay.write_text('AirPlay fixture')
        for name in ['sendspin-3.service', 'sendspin-9.tmp']:
            (self.services / name).write_text('stale')
        fake = self.root / 'chroot'
        fake.write_text('#!/bin/sh\ntrap "exit 0" TERM INT\ntrap : HUP\nwhile :; do sleep .05; done\n')
        fake.chmod(0o755)
        self.path = self.root / 'mdns.sock'
        binary = self.root / 'mdnsd'
        subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                        f'-DMDNS_CHROOT="{fake}"', f'-DMDNS_OWNER_EXE="{esphome_owner}"',
                        f'-DMDNS_SENDSPIN_OWNER_EXE="{sendspin_owner}"',
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

    def records(self, kind='sendspin'):
        return sorted(self.services.glob(f'{kind}-*.service'))

    def test_publish_render_single_owner_and_withdraw(self):
        self.start(os.path.realpath(sys.executable))
        # Cold start removed stale Sendspin records but not static ones.
        self.assertEqual(sorted(p.name for p in self.services.iterdir()), ['airplay.service'])
        name = 'Kitchen café & <Echo> "one"'
        with self.connect(sendspin_packet(name=name)) as owner:
            self.assertEqual(owner.recv(64), b'pending\n')
            self.assertEqual(len(self.records()), 1)
            parsed = ET.parse(self.records()[0]).getroot()
            self.assertEqual(parsed.findtext('name'), name)
            self.assertEqual(parsed.findtext('service/type'), '_sendspin._tcp')
            self.assertEqual(parsed.findtext('service/port'), '8928')
            txt = [x.text for x in parsed.findall('service/txt-record')]
            self.assertEqual(txt, ['path=/sendspin', 'name=' + name])
            # One record per kind: a second owner connection is refused.
            with self.connect(sendspin_packet(port=9000)) as second:
                self.assertEqual(second.recv(64), b'error\n')
            # The owner may re-register (new port); still a single record.
            owner.sendall(sendspin_packet(port=9001, name='Kitchen'))
            self.assertEqual(owner.recv(64), b'pending\n')
            self.assertEqual(len(self.records()), 1)
            self.assertEqual(ET.parse(self.records()[0]).getroot().findtext('service/port'), '9001')
        wait_for(self, lambda: not self.records())
        self.assertTrue(self.airplay.exists())

    def test_other_executable_cannot_advertise_sendspin(self):
        self.start('/run/libreecho/features/airplay2/root/usr/local/sbin/libreecho-sendspind')
        with self.connect(sendspin_packet()) as owner:
            self.assertEqual(owner.recv(64), b'error\n')
        self.assertEqual(self.records(), [])

    def test_malformed_registrations_rejected(self):
        self.start(os.path.realpath(sys.executable))
        bad = [
            b'SENDSPIN/1 8928\n',                                   # no name
            sendspin_packet(raw=''),                                # empty name
            sendspin_packet(port=0),
            sendspin_packet(port=65536),
            sendspin_packet(name='x' * 64),                         # over 63 bytes
            sendspin_packet(raw='4b0a6974'),                        # control character
            sendspin_packet(raw='4b006974'),                        # embedded NUL
            sendspin_packet(raw='zz'),                              # not hex
            sendspin_packet(raw='c328'),                            # invalid UTF-8
            b'SENDSPIN/1 8928 name=4b path=2f78\n',                 # extra key
            b'SENDSPIN/1 8928 name=4b',                             # unterminated
        ]
        for data in bad:
            with self.subTest(data=data), self.connect(data) as owner:
                self.assertEqual(owner.recv(64), b'error\n')
        self.assertEqual(self.records(), [])

    def test_coexists_with_esphome_lease(self):
        # Distinct owner executables: the interpreter and a copy of it.
        copy = Path(tempfile.mkdtemp()) / 'esphome-owner-python'
        self.addCleanup(shutil.rmtree, copy.parent)
        shutil.copy2(os.path.realpath(sys.executable), copy)
        self.start(os.path.realpath(sys.executable), esphome_owner=str(copy))
        helper = ('import socket,sys,time\n'
                  's=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET);s.connect(sys.argv[1])\n'
                  's.sendall(bytes.fromhex(sys.argv[2]));print(s.recv(64).decode().strip(),flush=True)\n'
                  'sys.stdin.read()\n')
        esphome = subprocess.Popen([str(copy), '-c', helper, str(self.path), esphome_packet().hex()],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(esphome.stdout.readline().strip(), 'pending')
            with self.connect(sendspin_packet()) as owner:
                self.assertEqual(owner.recv(64), b'pending\n')
                self.assertEqual(len(self.records('esphome')), 1)
                self.assertEqual(len(self.records('sendspin')), 1)
            # Losing the Sendspin owner withdraws only its record.
            wait_for(self, lambda: not self.records('sendspin'))
            self.assertEqual(len(self.records('esphome')), 1)
        finally:
            esphome.stdin.close()
            esphome.wait(timeout=5)
            esphome.stdout.close()
        wait_for(self, lambda: not self.records('esphome'))


if __name__ == '__main__':
    unittest.main()
