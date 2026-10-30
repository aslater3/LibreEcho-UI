#!/usr/bin/env python3
"""airplayd supervises the Sendspin player (sendspind) beside the shared engine.

Runs the real airplayd, built with a fake chroot helper, against a scratch
runtime root holding a fake engine (serving the Sendspin sink socket) and a
fake player that records each start. No root, mounts, network or hardware.

Checks: disabled by default; --sendspin-on-start starts it, from the mounted
payload, with production arguments only (name and state file); set_sendspin_enabled stops/starts it; an older payload without
the player reports unavailable and refuses enable without disturbing the
engine; a crashing player backs off; an engine restart stops the player and
starts a fresh one; the AirPlay receiver toggle leaves it running; shutdown
stops it.
"""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ['src/adapter/airplayd.c', 'src/adapter/mdns_client.c', 'src/adapter/mdns_lease.c',
           'src/adapter/adapter_client.c', 'src/adapter/adapter_server.c', 'src/log.c']

FAKE_CHROOT = '''#!/bin/sh
[ "$1" = chroot ] || exit 2
root=$2; cmd=$3; shift 3
LE_FAKE_ROOT=$root exec "$root$cmd" "$@"
'''
FAKE_ENGINE = '''#!/usr/bin/env python3
import os, socket, time
root = os.environ['LE_FAKE_ROOT']
audio = os.path.join(root, 'run/libreecho-audio')
os.makedirs(audio, exist_ok=True)
for name in ('sendspin.sock',):
    try: os.unlink(os.path.join(audio, name))
    except FileNotFoundError: pass
sink = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
sink.bind(os.path.join(audio, 'sendspin.sock'))
open(os.path.join(audio, 'media.pcm'), 'w').close()
open(os.path.join(root, 'engine.pid'), 'w').write(str(os.getpid()))
while True: time.sleep(1)
'''
# The player runs directly from the mounted payload (no chroot).
FAKE_PLAYER = '''#!/bin/sh
root=${0%/usr/local/sbin/libreecho-sendspind}
echo "$$ $*" >> "$root/sendspind.log"
[ -e "$root/crash" ] && exit 1
exec sleep 1000
'''
STATE_FILE = '/tmp/le-ss-svc-state-never-written'


def build_airplayd(work, chroot):
    binary = work / 'airplayd'
    subprocess.run(['cc', '-std=c99', '-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE',
                    f'-DAIRPLAY_CHROOT="{chroot}"', f'-DSENDSPIN_STATE_FILE="{STATE_FILE}"',
                    '-Isrc', '-Isrc/adapter', '-O1',
                    *SOURCES, '-o', str(binary)], cwd=ROOT, check=True, timeout=180)
    return binary


def call(path, cmd, args=None, timeout=5.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(str(path))
    s.sendall((json.dumps({'v': 1, 'id': 1, 'cmd': cmd, 'args': args or {}}) + '\n').encode())
    data = b''
    while b'\n' not in data:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    s.close()
    return json.loads(data)


def wait(predicate, timeout=10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.1)
    return predicate()


def alive(pid):
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    try:  # a zombie is not alive
        return Path(f'/proc/{pid}/stat').read_text().split()[2] != 'Z'
    except OSError:
        return False


class SendspinService(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='le-ss-svc-')
        work = Path(cls.tmp.name)
        chroot = work / 'fakechroot'
        chroot.write_text(FAKE_CHROOT)
        chroot.chmod(0o755)
        cls.binary = build_airplayd(work, chroot)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def start(self, *extra, player=True):
        self.run_dir = Path(tempfile.mkdtemp(prefix='r', dir=self.tmp.name))
        self.root = self.run_dir / 'root'
        sbin = self.root / 'usr/local/sbin'
        sbin.mkdir(parents=True)
        (self.root / 'run/libreecho-audio').mkdir(parents=True)
        files = {'libreecho-audio-engine': FAKE_ENGINE}
        if player:
            files['libreecho-sendspind'] = FAKE_PLAYER
        for name, body in files.items():
            (sbin / name).write_text(body)
            (sbin / name).chmod(0o755)
        self.sock = self.run_dir / 'airplay.sock'
        self.proc = subprocess.Popen(
            [str(self.binary), '--foreground', '--socket', str(self.sock), '--root', str(self.root),
             '--mdns-socket', str(self.run_dir / 'no-mdns.sock'), *extra],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(self.stop)
        self.assertTrue(wait(lambda: self.sock.exists() and self.status() is not None, 15),
                        'airplayd never answered')

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        for pid in self.engine_pids() + self.player_pids():
            if alive(pid):
                os.kill(pid, signal.SIGKILL)

    def status(self):
        try:
            reply = call(self.sock, 'status')
        except OSError:
            return None
        return reply.get('data') if reply.get('ok') else None

    def starts(self):
        log = self.root / 'sendspind.log'
        return log.read_text().splitlines() if log.exists() else []

    def player_pids(self):
        return [int(line.split()[0]) for line in self.starts()]

    def engine_pids(self):
        path = self.root / 'engine.pid'
        return [int(path.read_text())] if path.exists() and path.read_text() else []

    def running_player(self):
        pids = [p for p in self.player_pids() if alive(p)]
        return pids[-1] if pids else None

    def test_disabled_by_default(self):
        self.start()
        time.sleep(2)
        st = self.status()
        self.assertTrue(st['sendspin_available'])
        self.assertFalse(st['sendspin_enabled'])
        self.assertFalse(st['sendspin_running'])
        self.assertEqual(self.starts(), [])

    def test_on_start_runs_production_arguments_and_toggles(self):
        self.start('--sendspin-on-start')
        self.assertTrue(wait(lambda: self.running_player() is not None), 'player never started')
        args = self.starts()[-1].split()[1:]
        self.assertEqual(args[:3], ['--run', '--quiet', '--name'])
        self.assertEqual(args[4:], ['--state-file', STATE_FILE])
        self.assertNotIn('--dev-allow-unpaired', args)
        self.assertTrue(self.status()['sendspin_running'])

        first = self.running_player()
        self.assertTrue(call(self.sock, 'set_sendspin_enabled', {'enabled': False})['ok'])
        self.assertTrue(wait(lambda: not alive(first)), 'disable did not stop the player')
        self.assertFalse(self.status()['sendspin_enabled'])
        self.assertTrue(call(self.sock, 'set_sendspin_enabled', {'enabled': True})['ok'])
        self.assertTrue(wait(lambda: self.running_player() not in (None, first)),
                        're-enable did not start a fresh player')

    def test_airplay_receiver_toggle_leaves_player_running(self):
        self.start('--sendspin-on-start')
        self.assertTrue(wait(lambda: self.running_player() is not None))
        pid = self.running_player()
        call(self.sock, 'set_enabled', {'enabled': False})
        time.sleep(1.5)
        self.assertTrue(alive(pid), 'AirPlay receiver disable stopped Sendspin')
        self.assertEqual(len(self.starts()), 1)

    def test_old_payload_without_player(self):
        self.start('--sendspin-on-start', player=False)
        time.sleep(2)
        st = self.status()
        self.assertFalse(st['sendspin_available'])
        self.assertFalse(st['sendspin_running'])
        self.assertTrue(st['engine_running'], 'missing player disturbed the engine')
        reply = call(self.sock, 'set_sendspin_enabled', {'enabled': True})
        self.assertFalse(reply['ok'])
        self.assertIn('not installed', reply['error'])
        self.assertTrue(call(self.sock, 'set_sendspin_enabled', {'enabled': False})['ok'])

    def test_crashing_player_backs_off(self):
        self.start()
        (self.root / 'crash').touch()
        self.assertTrue(call(self.sock, 'set_sendspin_enabled', {'enabled': True})['ok'])
        time.sleep(6.5)
        count = len(self.starts())
        # 1 s, 2 s, 4 s backoff: at most 4 starts in 6.5 s, never a spin.
        self.assertGreaterEqual(count, 2, 'crashed player was not retried')
        self.assertLessEqual(count, 4, f'crash loop without backoff ({count} starts)')
        self.assertTrue(self.status()['engine_running'])

    def test_engine_restart_restarts_player(self):
        self.start('--sendspin-on-start')
        self.assertTrue(wait(lambda: self.running_player() is not None))
        player, engine = self.running_player(), self.engine_pids()[0]
        os.kill(engine, signal.SIGKILL)
        self.assertTrue(wait(lambda: not alive(player)), 'player kept running without its engine')
        self.assertTrue(wait(lambda: self.engine_pids() and self.engine_pids()[0] != engine, 15),
                        'engine not restarted')
        self.assertTrue(wait(lambda: self.running_player() not in (None, player), 15),
                        'player not restarted against the new engine')

    def pairing_state(self):
        state = Path(tempfile.mkdtemp(prefix='s', dir=self.tmp.name)) / 'sendspin-state'
        state.write_text('libreecho-sendspin-state 1\n')
        return state

    def test_reset_pairing_forgets_state_and_restarts(self):
        state = self.pairing_state()
        self.start('--sendspin-on-start', '--sendspin-state', str(state))
        self.assertTrue(wait(lambda: self.running_player() is not None))
        first = self.running_player()
        self.assertTrue(call(self.sock, 'reset_sendspin_pairing')['ok'])
        # The player is stopped before its state goes, so it cannot rewrite it.
        self.assertFalse(alive(first), 'player kept running across the reset')
        self.assertFalse(state.exists(), 'pairing state survived the reset')
        self.assertTrue(wait(lambda: self.running_player() not in (None, first)),
                        'player not restarted after the reset')

    def test_reset_pairing_while_disabled(self):
        state = self.pairing_state()
        self.start('--sendspin-state', str(state))
        self.assertTrue(call(self.sock, 'reset_sendspin_pairing')['ok'])
        self.assertFalse(state.exists(), 'pairing state survived the reset')
        time.sleep(1.5)
        self.assertEqual(self.starts(), [], 'reset started a disabled player')
        # Nothing to forget is still a successful reset.
        self.assertTrue(call(self.sock, 'reset_sendspin_pairing')['ok'])

    def test_shutdown_stops_player(self):
        self.start('--sendspin-on-start')
        self.assertTrue(wait(lambda: self.running_player() is not None))
        pid = self.running_player()
        self.proc.terminate()
        self.proc.wait(timeout=15)
        self.assertTrue(wait(lambda: not alive(pid), 5), 'player survived airplayd shutdown')


if __name__ == '__main__':
    unittest.main()
