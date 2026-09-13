"""Ordinary backup recovery and injected I/O failures; bwrap wrapper only."""
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import tarfile
import time
import unittest
import urllib.request

DATA = Path('/data/libreecho')
ARCHIVE = Path('/out/live-backup.tar.gz')
TOOL = ['/bin/sh', '/src/tools/libreecho-backup.sh']
RUNNING = ['libreecho-watchdogd', 'libreecho-web', 'libreecho-agentd',
           'libreecho-timerd', 'libreecho-ledd']
STUB = '''#!/bin/sh
name=${0##*/}
state=/run/states/$name
printf '%s %s\\n' "$name" "$1" >>/run/service-ops.log
case "$1" in
    status)
        test -f "$state" && exit 0
        case "$name" in
            libreecho-watchdogd|libreecho-radiod) exit 3 ;;
            *) exit 1 ;;
        esac
        ;;
    stop)
        test ! -f /run/stop-fail-$name || exit 1
        test ! -f /run/stay-running-$name || exit 0
        rm -f "$state" ;;
    start)
        test ! -f /run/start-fail-$name || exit 1
        touch "$state" ;;
    *) exit 2 ;;
esac
'''


def write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value)


class BackupRoundTrip(unittest.TestCase):
    def setUp(self):
        # This is intentionally not runnable against a normal host filesystem.
        self.assertEqual(str(Path.cwd()), '/src', 'use the bwrap shell wrapper')
        self.assertTrue(os.path.ismount('/data') and os.path.ismount('/out'))
        for directory in [DATA, Path('/etc/libreecho'), Path('/etc/init.d'),
                          Path('/run/states'), Path('/tmp/faultbin')]:
            shutil.rmtree(directory, ignore_errors=True)
            directory.mkdir(parents=True)
        for path in Path('/run').iterdir():
            if path.is_file():
                path.unlink()
        for path in Path('/out').iterdir():
            if path.is_file():
                path.unlink()
        self.files = {
            'config/web-config.json': '{"hostname":"live-user-configured"}\n',
            'config/users': 'fixture-user:sha256:00112233445566778899aabbccddeeff:'
                            'acb0f5de20ce91fa7dd3df21aa90fa43ae3b7a69ca335cf025f8aaef0dacec4c\n',
            'config/web-config.json.setup-complete': 'schema=1\n',
            'config/timers': 'timer-live\n',
            'config/agent.json': '{"provider":"fixture"}\n',
            'config/bluetooth.devices': 'bond-live\n',
            'config/bluetooth.keys': 'key-live\n',
            'config/led-state.json': '{"brightness":42}\n',
            'config/ntp.conf': 'server time.example.test\n',
            'config/tts-voice': 'southern-female\n',
            'config/nested/setting': 'nested-live\n',
            'secrets/openai-codex.json': 'synthetic-noncredential\n',
        }
        self.excluded = ['config/wake-dump.raw', 'config/web-config.json.tmp',
                         'config/users.new', 'config/vendor-import-force-next-boot']
        for relative, value in self.files.items():
            write(DATA / relative, value)
        for relative in self.excluded:
            write(DATA / relative, 'fixture-excluded\n')
        self.preserved = {
            '/etc/libreecho/web-config.json': '{"hostname":"shipped-default"}\n',
            str(DATA / 'features/assistant/payload.squashfs'): 'feature-fixture\n',
            str(DATA / 'update/state'): 'ota-fixture\n',
            str(DATA / 'data-manifest.json'): 'release-fixture\n',
        }
        for path, value in self.preserved.items():
            write(path, value)
        for name in RUNNING + ['libreecho-micd', 'libreecho-waked',
                               'libreecho-radiod']:
            path = Path('/etc/init.d') / name
            write(path, STUB)
            path.chmod(0o755)
        for name in RUNNING:
            (Path('/run/states') / name).touch()
        # Never issue a host-wide sync syscall from a fixture.
        self.inject('sync', 'printf "sync\\n" >>/run/sync.log\nexit 0\n')
        self.env = dict(os.environ, PATH='/tmp/faultbin:/usr/bin:/bin')

    def inject(self, name, body):
        path = Path('/tmp/faultbin') / name
        write(path, '#!/bin/sh\n' + body)
        path.chmod(0o755)

    def call(self, action, path=ARCHIVE, success=True, extra_env=None):
        env = dict(self.env, **(extra_env or {}))
        result = subprocess.run(TOOL + [action, str(path)], input='y\n',
                                text=True, capture_output=True, env=env, timeout=15)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, 'operation falsely returned success')
            self.assertNotIn('restore complete', result.stdout)
            self.assertNotIn('backup created', result.stdout)
        return result

    def actions(self, action):
        path = Path('/run/service-ops.log')
        if not path.exists():
            return []
        return [line.split()[0] for line in path.read_text().splitlines()
                if line.split()[1] == action]

    def test_roundtrip_replaces_changed_state_and_preserves_exclusions(self):
        self.call('create')
        for relative in self.files:
            write(DATA / relative, 'changed-after-backup\n')
        write(DATA / 'config/not-in-backup', 'must-be-removed\n')
        self.call('restore')
        for relative, value in self.files.items():
            path = DATA / relative
            self.assertTrue(path.read_text() == value, 'restored bytes differ: ' + relative)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600, relative)
        for root in [DATA / 'config', DATA / 'secrets']:
            for path in [root] + list(root.rglob('*')):
                if path.is_dir():
                    self.assertEqual(path.stat().st_mode & 0o777, 0o700, str(path))
        for relative in self.excluded + ['config/not-in-backup']:
            self.assertFalse((DATA / relative).exists(), relative)
        for path, value in self.preserved.items():
            self.assertTrue(Path(path).read_text() == value, 'excluded state changed: ' + path)
        self.assertEqual(ARCHIVE.stat().st_mode & 0o777, 0o600)
        with tarfile.open(ARCHIVE, 'r:gz') as archive:
            members = {member.name: member for member in archive.getmembers()}
            expected = {'manifest.json', 'persistent', 'persistent/config',
                        'persistent/config/nested', 'persistent/secrets'}
            expected.update('persistent/' + relative for relative in self.files)
            self.assertEqual(set(members), expected)
            for name, member in members.items():
                self.assertEqual(member.mode & 0o777, 0o700 if member.isdir() else 0o600, name)
            manifest_file = archive.extractfile('manifest.json')
            assert manifest_file is not None
            manifest = json.load(manifest_file)
            self.assertEqual(manifest['version'], 2)
            self.assertEqual(manifest['scope'], 'active-persistent-state')
        self.assert_mock_consumes_restored_config()

    def assert_mock_consumes_restored_config(self):
        token = secrets.token_hex(24)
        write('/etc/auth-token', token + '\n')
        Path('/etc/auth-token').chmod(0o600)
        command = ['/src/build/libreecho-web', '--backend', 'mock',
                   '--config', str(DATA / 'config/web-config.json'),
                   '--users-file', str(DATA / 'config/users'),
                   '--auth-token-file', '/etc/auth-token', '--allow-insecure-lan',
                   '--web-root', '/src/web', '--listen', '127.0.0.1:18083']
        with open('/tmp/mock-server.log', 'w') as log:
            server = subprocess.Popen(command, stdout=log, stderr=log, env=self.env)
            try:
                request = urllib.request.Request('http://127.0.0.1:18083/api/v1/network',
                                                 headers={'Authorization': 'Bearer ' + token})
                for _ in range(50):
                    self.assertIsNone(server.poll(), 'mock server exited before readiness')
                    try:
                        with urllib.request.urlopen(request, timeout=1) as response:
                            result = json.load(response)
                        break
                    except OSError:
                        time.sleep(0.1)
                else:
                    self.fail('mock server did not become ready')
                self.assertTrue(result['ok'])
                self.assertEqual(result['data']['hostname'], 'live-user-configured')
            finally:
                server.terminate()
                try:
                    server.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=3)

    def test_missing_required_state_is_not_a_complete_backup(self):
        for relative in ['config/web-config.json', 'config/users', 'secrets']:
            with self.subTest(relative=relative):
                target = DATA / relative
                saved = Path('/data/required-saved')
                target.rename(saved)
                try:
                    self.call('create', success=False)
                    self.assertFalse(ARCHIVE.exists())
                finally:
                    saved.rename(target)

    def test_restore_only_restarts_previously_running_services(self):
        self.call('create')
        self.call('restore')
        stopped = self.actions('stop')
        self.assertEqual(set(stopped), set(RUNNING))
        self.assertEqual(stopped[0], 'libreecho-watchdogd')
        self.assertEqual(self.actions('start'), list(reversed(stopped)))
        self.assertFalse(Path('/run/states/libreecho-micd').exists())

    def test_restore_accepts_shipped_inactive_statuses_and_dependency_order(self):
        self.call('create')
        for name in ['libreecho-micd', 'libreecho-waked']:
            (Path('/run/states') / name).touch()
        self.call('restore')
        starts = self.actions('start')
        self.assertLess(starts.index('libreecho-micd'), starts.index('libreecho-waked'))
        self.assertLess(starts.index('libreecho-waked'), starts.index('libreecho-agentd'))
        self.assertEqual(starts[-1], 'libreecho-watchdogd')
        self.assertNotIn('libreecho-radiod', starts)

    def test_restore_preserves_existing_numeric_consumer_ownership(self):
        self.call('create')
        owner = f'{os.getuid()}:{os.getgid()}'
        self.call('restore', extra_env={
            'LIBREECHO_CONFIG_OWNER': owner,
            'LIBREECHO_SECRETS_OWNER': owner,
        })
        for path in [DATA / 'config', DATA / 'config/web-config.json',
                     DATA / 'secrets', DATA / 'secrets/openai-codex.json']:
            self.assertEqual(f'{path.stat().st_uid}:{path.stat().st_gid}', owner,
                             str(path))

    def test_invalid_configured_owner_fails_before_state_change(self):
        self.call('create')
        write(DATA / 'config/web-config.json', 'current-state\n')
        self.call('restore', success=False,
                  extra_env={'LIBREECHO_CONFIG_OWNER': 'not-a-numeric-owner'})
        self.assertEqual((DATA / 'config/web-config.json').read_text(),
                         'current-state\n')

    def test_stop_failure_preserves_state_and_resumes_stopped_services(self):
        self.call('create')
        write(DATA / 'config/web-config.json', 'current-state\n')
        Path('/run/stop-fail-libreecho-web').touch()
        self.call('restore', success=False)
        self.assertEqual((DATA / 'config/web-config.json').read_text(), 'current-state\n')
        self.assertTrue(Path('/run/states/libreecho-watchdogd').exists())
        self.assertIn('libreecho-watchdogd', self.actions('stop'))
        self.assertIn('libreecho-watchdogd', self.actions('start'))

    def test_stop_success_must_confirm_service_is_stopped(self):
        self.call('create')
        write(DATA / 'config/web-config.json', 'current-state\n')
        Path('/run/stay-running-libreecho-web').touch()
        self.call('restore', success=False)
        self.assertEqual((DATA / 'config/web-config.json').read_text(), 'current-state\n')

    def test_copy_failure_is_reported_without_restarting_partial_state(self):
        self.call('create')
        write(DATA / 'config/web-config.json', 'current-state\n')
        self.inject('cp', 'for arg do\n case "$arg" in *"/config.restore-stage."*) exit 7 ;; esac\ndone\nexec /bin/cp "$@"\n')
        self.call('restore', success=False)
        self.assertEqual((DATA / 'config/web-config.json').read_text(),
                         'current-state\n')
        self.assertEqual(self.actions('start'), [])

    def test_mode_failure_is_reported_without_restarting_partial_state(self):
        self.call('create')
        write(DATA / 'config/web-config.json', 'current-state\n')
        self.inject('chmod', 'for arg do\n case "$arg" in *"/config.restore-stage."*/users) exit 7 ;; esac\ndone\nexec /bin/chmod "$@"\n')
        self.call('restore', success=False)
        self.assertEqual((DATA / 'config/web-config.json').read_text(),
                         'current-state\n')
        self.assertEqual(self.actions('start'), [])

    def test_late_component_permission_failure_preserves_both_live_trees(self):
        self.call('create')
        write(DATA / 'config/web-config.json', 'current-config\n')
        write(DATA / 'secrets/openai-codex.json', 'current-secret\n')
        self.inject('chmod', 'for arg do\n case "$arg" in *"/secrets.restore-stage."*/openai-codex.json) exit 7 ;; esac\ndone\nexec /bin/chmod "$@"\n')
        self.call('restore', success=False)
        self.assertEqual((DATA / 'config/web-config.json').read_text(),
                         'current-config\n')
        self.assertEqual((DATA / 'secrets/openai-codex.json').read_text(),
                         'current-secret\n')
        self.assertEqual(self.actions('start'), [])

    def test_replacement_failure_rolls_back_both_live_trees(self):
        self.call('create')
        for component in ['config', 'secrets']:
            with self.subTest(component=component):
                write(DATA / 'config/web-config.json', 'current-config\n')
                write(DATA / 'secrets/openai-codex.json', 'current-secret\n')
                self.inject('mv', 'case "$1" in /data/libreecho/' + component +
                            '.restore-stage.*) exit 7 ;; esac\nexec /bin/mv "$@"\n')
                result = self.call('restore', success=False)
                self.assertEqual((DATA / 'config/web-config.json').read_text(),
                                 'current-config\n')
                self.assertEqual((DATA / 'secrets/openai-codex.json').read_text(),
                                 'current-secret\n')
                self.assertEqual(list(DATA.glob('*.restore-backup.*')), [])
                self.assertEqual(list(DATA.glob('*.restore-stage.*')), [])
                self.assertIn('restore replacement failed', result.stderr)
                self.assertEqual(self.actions('start'), [])

    def test_failed_rollback_keeps_saved_original_for_manual_recovery(self):
        self.call('create')
        write(DATA / 'config/web-config.json', 'current-config\n')
        write(DATA / 'secrets/openai-codex.json', 'current-secret\n')
        # Model ordinary rename errors, not filesystem corruption or archives.
        self.inject('mv', 'case "$1" in\n'
                    ' /data/libreecho/secrets|/data/libreecho/config.restore-backup.*) exit 7 ;;\n'
                    'esac\nexec /bin/mv "$@"\n')
        result = self.call('restore', success=False)
        saved = list(DATA.glob('config.restore-backup.*'))
        self.assertEqual(len(saved), 1, 'rollback failure deleted the saved original')
        self.assertEqual((saved[0] / 'web-config.json').read_text(), 'current-config\n')
        self.assertEqual((DATA / 'secrets/openai-codex.json').read_text(),
                         'current-secret\n')
        self.assertIn(str(saved[0]), result.stderr)
        self.assertEqual(list(DATA.glob('*.restore-stage.*')), [])
        self.assertEqual(self.actions('start'), [])

    def test_sync_failure_is_reported_without_restarting_partial_state(self):
        self.call('create')
        self.inject('sync', 'exit 7\n')
        self.call('restore', success=False)
        self.assertEqual(self.actions('start'), [])

    def test_service_restart_failure_is_not_success(self):
        self.call('create')
        Path('/run/start-fail-libreecho-web').touch()
        self.call('restore', success=False)


if __name__ == '__main__':
    unittest.main(verbosity=2)
