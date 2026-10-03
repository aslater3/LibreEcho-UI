"""Isolated real seqpacket lease/rendering tests; no host discovery daemons."""
import array
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
FIELDS = dict(version='2026.9.0', mac='02:00:00:00:00:01', platform='LibreEcho',
              board='radar-puffin', network='wifi', friendly_name='Kitchen café & <Echo> "one"',
              api_encryption='Noise_NNpsk0_25519_ChaChaPoly_SHA256', project_name='LibreEcho', project_version='0.14.0')

def packet(port=6053, fields=None):
    fields = FIELDS if fields is None else fields
    return ('ESPHOME/1 ' + str(port) + ''.join(' ' + k + '=' + v.encode().hex()
            for k, v in fields.items()) + '\n').encode()

def wait_for(test, condition):
    end = time.monotonic() + 4
    while not condition() and time.monotonic() < end:
        time.sleep(.02)
    test.assertTrue(condition())

class ESPHome(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.services = self.root / 'etc/avahi/services'
        self.services.mkdir(parents=True)
        (self.root / 'run').mkdir()
        self.airplay = self.services / 'airplay.service'
        self.airplay.write_text('AirPlay fixture: not owned by HA lease')
        for name in ['wyoming.service', 'wyoming-12.service', 'esphome.service', 'esphome-2.service']:
            (self.services / name).write_text('stale')
        fake = self.root / 'chroot'
        fake.write_text('#!/bin/sh\ntrap "exit 0" TERM INT\ntrap : HUP\nwhile :; do sleep .05; done\n')
        fake.chmod(0o755)
        self.path = self.root / 'mdns.sock'
        self.binary = self.root / 'mdnsd'
        self.compile(os.path.realpath(sys.executable), fake)
        self.proc = subprocess.Popen([str(self.binary), '--root', str(self.root), '--socket', str(self.path)])
        wait_for(self, self.path.exists)

    def compile(self, owner, fake):
        subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-DMDNS_CHROOT="' + str(fake) + '"', '-DMDNS_OWNER_EXE="' + owner + '"',
                        str(ROOT / 'src/adapter/mdnsd.c'), str(ROOT / 'src/adapter/mdns_lease.c'),
                        '-o', str(self.binary)], check=True, capture_output=True, timeout=30)

    def tearDown(self):
        self.proc.terminate()
        self.proc.wait(timeout=4)
        self.temp.cleanup()

    def connect(self, data):
        owner = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        owner.settimeout(3)
        owner.connect(str(self.path))
        owner.sendall(data)
        return owner

    def ha_records(self):
        return list(self.services.glob('esphome-*.service'))

    def test_unicode_xml_render_single_owner_and_eof(self):
        with self.connect(packet()) as owner:
            self.assertEqual(owner.recv(64), b'pending\n')
            self.assertEqual(len(self.ha_records()), 1)
            parsed = ET.parse(self.ha_records()[0]).getroot()
            self.assertEqual(parsed.findtext('name'), FIELDS['friendly_name'])
            self.assertEqual(parsed.findtext('service/type'), '_esphomelib._tcp')
            self.assertEqual(parsed.findtext('service/port'), '6053')
            self.assertEqual(dict(x.text.split('=', 1) for x in parsed.findall('service/txt-record')), FIELDS)
            with self.connect(packet()) as second:
                self.assertEqual(second.recv(64), b'error\n')
            owner.sendall(packet(65535))
            self.assertEqual(owner.recv(64), b'pending\n')
            self.assertEqual(len(self.ha_records()), 1)
        wait_for(self, lambda: not self.ha_records())
        self.assertTrue(self.airplay.exists())

    def test_cold_start_removes_only_stale_ha_records(self):
        self.assertEqual(list(self.services.iterdir()), [self.airplay])

    def test_airplay_file_toggles_do_not_change_ha_lease(self):
        with self.connect(packet()) as owner:
            self.assertEqual(owner.recv(64), b'pending\n')
            record = self.ha_records()[0]
            body = record.read_bytes()
            self.airplay.unlink()
            time.sleep(.15)
            self.assertEqual(self.ha_records(), [record])
            self.assertEqual(record.read_bytes(), body)
            self.airplay.write_text('AirPlay on again')
            time.sleep(.15)
            self.assertEqual(self.ha_records(), [record])
            self.assertEqual(record.read_bytes(), body)
            self.assertIsNone(self.proc.poll())
        wait_for(self, lambda: not self.ha_records())
        self.assertEqual(self.airplay.read_text(), 'AirPlay on again')

    def test_invalid_fields_ports_and_obsolete_verb(self):
        bad = [b'WYOMING/1 10700\n', packet(0), packet(65536), packet(-1), packet('1x'),
               packet().replace(b'ESPHOME/1 ', b'ESPHOME/2 '), packet() + b'junk',
               packet().replace(b'\n', b' version=31\n'),
               packet().replace(b'\n', b' arbitrary=31\n'),
               packet().replace(b'\n', b' ' + b'k'*512 + b'=31\n'),
               packet().replace(b'version=', b'version=f'),
               packet().replace(b'version=', b'version=00'),
               packet().replace(b'\n', b'\x00\n')]
        for field, value in [('platform', 'ESP32'), ('board', 'wrong'), ('api_encryption', 'none'),
                             ('api_encryption', 'Noise'),
                             ('mac', '00:00:00:00:00:00'), ('mac', '02:00:00:00:00:XX'),
                             ('friendly_name', 'x'*64), ('friendly_name', 'é'*32), ('version', 'x'*32),
                             ('friendly_name', 'bad\nname')]:
            bad.append(packet(fields=dict(FIELDS, **{field: value})))
        bad.append(packet(fields={k: v for k, v in FIELDS.items() if k != 'mac'}))
        bad.append(packet().replace(FIELDS['friendly_name'].encode().hex().encode(), b'c080'))
        bad.append(b'ESPHOME/1 6053 ' + b'x'*4096 + b'\n')
        for data in bad:
            with self.subTest(data=data[:90]), self.connect(data) as owner:
                self.assertEqual(owner.recv(64), b'error\n')
                self.assertFalse(self.ha_records())
        with self.connect(packet(1)) as owner:
            self.assertEqual(owner.recv(64), b'pending\n')
        wait_for(self, lambda: not self.ha_records())

    def test_unallowlisted_executable_cannot_claim_lease(self):
        # An executable that is not the selected ESPHome owner is refused,
        # even with exactly the same uid and a valid record.
        self.proc.terminate(); self.proc.wait(timeout=4)
        self.compile('/bin/sleep', self.root / 'chroot')
        self.proc = subprocess.Popen([str(self.binary), '--root', str(self.root), '--socket', str(self.path)])
        wait_for(self, self.path.exists)
        with self.connect(packet()) as owner:
            self.assertEqual(owner.recv(64), b'error\n')
        self.assertFalse(self.ha_records())
        # argv[0] cannot substitute for the kernel's executable identity.
        program = 'import socket,sys; s=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET); s.settimeout(3); s.connect(sys.argv[1]); s.sendall(bytes.fromhex(sys.argv[2])); sys.stdout.buffer.write(s.recv(64))'
        forged = subprocess.run(['/bin/sleep', '-c', program, str(self.path), packet().hex()],
                                executable=sys.executable, capture_output=True, timeout=4)
        self.assertEqual(forged.returncode, 0, forged.stderr)
        self.assertEqual(forged.stdout, b'error\n')
        self.assertFalse(self.ha_records())

    def test_live_peer_death_with_inherited_socket_withdraws(self):
        left, right = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        program = '''import array, socket, sys, time
s=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET)
s.connect(sys.argv[1]);s.sendall(bytes.fromhex(sys.argv[3]))
r=s.recv(64)
p=socket.socket(fileno=int(sys.argv[2]))
p.sendmsg([r],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',[s.fileno()]))])
time.sleep(10)
'''
        child = subprocess.Popen([sys.executable, '-c', program, str(self.path), str(right.fileno()), packet().hex()],
                                 pass_fds=(right.fileno(),))
        held = None
        try:
            left.settimeout(3)
            data, anc, _, _ = left.recvmsg(64, socket.CMSG_SPACE(4))
            self.assertEqual(data, b'pending\n')
            fd = array.array('i'); fd.frombytes(anc[0][2][:4])
            held = socket.socket(fileno=fd[0])
            self.assertEqual(len(self.ha_records()), 1)
            child.kill(); child.wait(timeout=3)
            # The socket is still open here; EOF-only withdrawal is insufficient.
            wait_for(self, lambda: not self.ha_records())
            self.assertTrue(self.airplay.exists())
        finally:
            if child.poll() is None:
                child.kill(); child.wait(timeout=3)
            if held is not None:
                held.close()
            left.close(); right.close()

if __name__ == '__main__':
    unittest.main()
