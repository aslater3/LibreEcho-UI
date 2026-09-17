"""Ordinary backup recovery and injected I/O failures; bwrap wrapper only."""
import io
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
# Spellings of one configured root. `test -L` follows only a *final* component,
# so a trailing or doubled separator, and every `.`/`..` alias of the root, must
# still reach the refusal instead of archiving whatever the link points at.
ROOT_ALIASES = [('', 'symbolic link'), ('/', 'symbolic link'),
                ('//', 'symbolic link'), ('/.', 'dot component'),
                ('/./', 'dot component'), ('/.//', 'dot component'),
                ('/..', 'dot component'), ('/../.', 'dot component')]
EXCLUSION_NOTICE = ('Excluded by contract, never restored: transaction files '
                    '(*.tmp, *.new, *.bak) and one-shot wake or vendor markers')
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
                         'config/users.new', 'config/vendor-import-force-next-boot',
                         'config/wake-dump-seconds',
                         'config/web-config.json.bak',
                         'secrets/openai-codex.json.bak']
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

    def test_diagnostics_never_expose_captured_bytes(self):
        # The archive legitimately contains credentials and account state, so no
        # operation may echo those bytes back into its output or the manifest.
        secret = 'fixture-credential-' + secrets.token_hex(16)
        account = 'fixture-account-' + secrets.token_hex(16)
        write(DATA / 'secrets/openai-codex.json', secret + '\n')
        write(DATA / 'config/users', account + '\n')
        results = {'create': self.call('create'), 'list': self.call('list'),
                   'restore': self.call('restore')}
        for name, result in results.items():
            with self.subTest(action=name):
                for value in [secret, account]:
                    self.assertNotIn(value, result.stdout)
                    self.assertNotIn(value, result.stderr)
        with tarfile.open(ARCHIVE, 'r:gz') as archive:
            manifest_file = archive.extractfile('manifest.json')
            assert manifest_file is not None
            manifest = json.dumps(json.load(manifest_file))
        self.assertNotIn(secret, manifest)
        self.assertNotIn(account, manifest)

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

    def stage_archive(self, name, build):
        """Build /out/<name> from a callback that fills a fresh staging tree."""
        stage = Path('/out/archive-stage')
        shutil.rmtree(stage, ignore_errors=True)
        stage.mkdir(parents=True)
        build(stage)
        archive = Path('/out') / name
        with tarfile.open(archive, 'w:gz') as handle:
            for member in sorted(stage.rglob('*')):
                handle.add(member, arcname=str(member.relative_to(stage)))
        return archive

    def write_basic_trees(self, stage, prefix='persistent'):
        for relative in ['config/web-config.json', 'config/users',
                         'secrets/openai-codex.json']:
            write(stage / prefix / relative, self.files[relative])

    def hostile_archive(self):
        """A structurally valid archive that also plants a symlink."""
        stage = Path('/out/hostile-stage')
        shutil.rmtree(stage, ignore_errors=True)
        stage.mkdir(parents=True)
        for relative in ['config/web-config.json', 'config/users',
                         'secrets/openai-codex.json']:
            write(stage / 'persistent' / relative, self.files[relative])
        # Points back at the factory seed: restored state must never be a
        # reference to another tree, however valid its target looks.
        os.symlink('/etc/libreecho/web-config.json',
                   stage / 'persistent/config/escape-link')
        write(stage / 'manifest.json', '{"version":2}\n')
        hostile = Path('/out/hostile-backup.tar.gz')
        with tarfile.open(hostile, 'w:gz') as archive:
            for member in sorted(stage.rglob('*')):
                archive.add(member, arcname=str(member.relative_to(stage)))
        return hostile

    def test_symlinked_state_is_refused_in_both_directions(self):
        # A symlink in the live trees must not become a backup...
        os.symlink('/etc/libreecho/web-config.json', DATA / 'config/live-link')
        result = self.call('create', success=False)
        self.assertIn('symbolic link', result.stderr)
        self.assertFalse(ARCHIVE.exists())
        (DATA / 'config/live-link').unlink()

        # ...and a symlink planted in an archive must not reach live state.
        self.call('create')
        result = self.call('restore', path=self.hostile_archive(), success=False)
        self.assertIn('symbolic link', result.stderr)
        self.assertEqual(self.actions('stop'), [],
                         'services were stopped for an archive refused up front')
        for relative, value in self.files.items():
            self.assertEqual((DATA / relative).read_text(), value,
                             'live state changed: ' + relative)
        self.assertFalse((DATA / 'config/escape-link').exists())
        self.assertIn('symbolic link', self.call(
            'list', path=self.hostile_archive(), success=False).stderr)

    def test_symlinked_state_root_is_refused(self):
        # A root that is itself a symlink is dereferenced by `cp -R` before the
        # staged tree can be scanned, so the configured root itself must be
        # refused instead of archiving whatever the link points at.
        self.call('create')
        before = ARCHIVE.stat()
        outside = Path('/out/outside-config')
        shutil.rmtree(outside, ignore_errors=True)
        outside.mkdir(parents=True)
        write(outside / 'web-config.json', '{"hostname":"outside-tree"}\n')
        write(outside / 'users', 'outside-account\n')
        live = Path('/data/config-live')
        shutil.rmtree(live, ignore_errors=True)
        (DATA / 'config').rename(live)
        os.symlink(str(outside), DATA / 'config')
        try:
            # A trailing or doubled separator, or a dot component, is another
            # spelling of the same root, not an escape from the check: `test -L`
            # resolves the final component of each through the link.
            for suffix, reason in ROOT_ALIASES:
                with self.subTest(config_root=str(DATA / 'config') + suffix):
                    env = {'LIBREECHO_CONFIG_DIR':
                           str(DATA / 'config') + suffix}
                    created = self.call('create', success=False, extra_env=env)
                    self.assertIn(reason, created.stderr)
                    after = ARCHIVE.stat()
                    self.assertEqual((after.st_size, after.st_mtime_ns),
                                     (before.st_size, before.st_mtime_ns),
                                     'refused create overwrote the archive')
                    restored = self.call('restore', success=False, extra_env=env)
                    self.assertIn(reason, restored.stderr)
                    self.assertEqual(self.actions('stop'), [],
                                     'services were stopped for a refused restore')
                    self.assertTrue((DATA / 'config').is_symlink(),
                                    'the live config root was replaced')
                    self.assertEqual((outside / 'web-config.json').read_text(),
                                     '{"hostname":"outside-tree"}\n')
        finally:
            (DATA / 'config').unlink()
            live.rename(DATA / 'config')

    def test_exclusions_match_exact_paths_not_basenames(self):
        # The wake diagnostics are excluded at their fixed config paths, not as
        # a name class: another file that merely shares a basename is committed
        # state and must survive a round trip.
        nested = DATA / 'config/nested/wake-dump-seconds'
        stray = DATA / 'secrets/wake-dump-seconds'
        write(nested, 'unrelated-nested-state\n')
        write(stray, 'unrelated-secret-state\n')
        self.call('create')
        with tarfile.open(ARCHIVE, 'r:gz') as archive:
            names = [member.name for member in archive.getmembers()]
        for kept in ['persistent/config/nested/wake-dump-seconds',
                     'persistent/secrets/wake-dump-seconds']:
            self.assertIn(kept, names)
        for pruned in ['persistent/config/wake-dump-seconds',
                       'persistent/config/wake-dump.raw',
                       'persistent/config/vendor-import-force-next-boot']:
            self.assertNotIn(pruned, names)
        self.call('restore')
        for path, value in [(nested, 'unrelated-nested-state\n'),
                            (stray, 'unrelated-secret-state\n')]:
            self.assertEqual(path.read_text(), value, 'restored bytes differ')
        self.assertFalse((DATA / 'config/wake-dump-seconds').exists())
        self.assertFalse((DATA / 'config/wake-dump.raw').exists())
        self.assertFalse((DATA / 'config/vendor-import-force-next-boot').exists())

    def test_symlinked_data_root_is_refused(self):
        # The data root itself as a link, spelled with a trailing slash: `test
        # -L` resolves the final component of `/data/libreecho/` through the
        # link, so an untrimmed spelling would archive the linked tree.
        self.call('create')
        before = ARCHIVE.stat()
        real = Path('/data/state-real')
        shutil.rmtree(real, ignore_errors=True)
        DATA.rename(real)
        os.symlink(str(real), DATA)
        try:
            for suffix, reason in ROOT_ALIASES:
                with self.subTest(data_root=str(DATA) + suffix):
                    env = {'LIBREECHO_DATA_ROOT': str(DATA) + suffix}
                    created = self.call('create', success=False, extra_env=env)
                    self.assertIn(reason, created.stderr)
                    restored = self.call('restore', success=False, extra_env=env)
                    self.assertIn(reason, restored.stderr)
                    self.assertEqual(self.actions('stop'), [],
                                     'services were stopped for a refused restore')
            after = ARCHIVE.stat()
            self.assertEqual((after.st_size, after.st_mtime_ns),
                             (before.st_size, before.st_mtime_ns),
                             'refused create overwrote the archive')
            self.assertEqual((real / 'config/web-config.json').read_text(),
                             self.files['config/web-config.json'])
        finally:
            DATA.unlink()
            real.rename(DATA)

    def test_symlinked_ancestor_component_is_refused(self):
        # Only a component *above* the configured root is a link, so the root
        # itself and its final component are plain directories: the check has to
        # inspect every component without following links, or the copy and the
        # restore land inside the link target.
        self.call('create')
        before = ARCHIVE.stat()
        outside = Path('/out/ancestor-target')
        shutil.rmtree(outside, ignore_errors=True)
        (outside / 'state/config').mkdir(parents=True)
        (outside / 'state/secrets').mkdir(parents=True)
        write(outside / 'state/config/web-config.json',
              '{"hostname":"ancestor-tree"}\n')
        write(outside / 'state/config/users', 'ancestor-account\n')
        write(outside / 'state/secrets/ancestor-secret', 'ancestor-secret\n')
        alias = Path('/data/alias')
        os.symlink(str(outside), alias)
        try:
            for env in [{'LIBREECHO_DATA_ROOT': str(alias / 'state')},
                        {'LIBREECHO_CONFIG_DIR': str(alias / 'state/config')}]:
                for action in ['create', 'restore']:
                    with self.subTest(action=action, **env):
                        result = self.call(action, success=False, extra_env=env)
                        self.assertIn('crosses a symbolic link', result.stderr)
                        self.assertIn(str(alias), result.stderr)
            # Nothing was staged into or written through the link target.
            self.assertEqual(sorted(path.name for path in
                                    (outside / 'state/config').iterdir()),
                             ['users', 'web-config.json'])
            self.assertEqual(sorted(path.name for path in
                                    (outside / 'state/secrets').iterdir()),
                             ['ancestor-secret'])
            after = ARCHIVE.stat()
            self.assertEqual((after.st_size, after.st_mtime_ns),
                             (before.st_size, before.st_mtime_ns),
                             'refused create overwrote the archive')
            self.assertEqual(self.actions('stop'), [],
                             'services were stopped for a refused restore')
            self.assertEqual((outside / 'state/config/users').read_text(),
                             'ancestor-account\n')
            self.assertEqual((outside / 'state/secrets/ancestor-secret').read_text(),
                             'ancestor-secret\n')
        finally:
            alias.unlink()

    def test_manifest_names_the_pruned_transaction_file_class(self):
        # A consumer auditing the archive must be able to tell an intentional
        # omission from an incomplete backup, so the manifest names every tree
        # and every suffix the pruning class removes, including the durable
        # stale pre-update copies `config_write_atomic` leaves as `*.bak`.
        write(DATA / 'secrets/provider.new', 'transaction-fixture\n')
        write(DATA / 'config/web-config.json.bak', 'stale-fixture\n')
        self.call('create')
        with tarfile.open(ARCHIVE, 'r:gz') as archive:
            manifest_file = archive.extractfile('manifest.json')
            assert manifest_file is not None
            excluded = json.load(manifest_file)['excluded']
            names = [member.name for member in archive.getmembers()]
        self.assertIn('transaction-files:config,secrets:*.tmp,*.new,*.bak',
                      excluded)
        self.assertNotIn('config-transaction-files:config/*.tmp', excluded)
        self.assertNotIn('persistent/secrets/provider.new', names)
        self.assertNotIn('persistent/config/web-config.json.bak', names)

    def test_dot_and_prefix_names_are_not_over_refused(self):
        # The refusal keys on a `/`-separated `.`/`..` component, not on a name
        # that merely contains a dot, and it must not confuse another root that
        # shares a name prefix with the configured one: both are ordinary state
        # and must still round-trip.
        sibling = Path('/data/state-other')
        outside = Path('/out/prefix-outside')
        shutil.rmtree(outside, ignore_errors=True)
        outside.mkdir(parents=True)
        write(outside / 'marker', 'prefix-confusion\n')
        os.symlink(str(outside), sibling)
        dotted = Path('/data/.state-dotted')
        DATA.rename(dotted)
        try:
            # The dotted name is a name, not a component, and the doubled
            # separator is normalized rather than treated as an alias.
            for spelling in [str(dotted), '/data//' + dotted.name]:
                with self.subTest(data_root=spelling):
                    created = self.call('create',
                                        extra_env={'LIBREECHO_DATA_ROOT': spelling})
                    self.assertNotIn('symbolic link', created.stderr)
                    self.assertNotIn('dot component', created.stderr)
                    self.call('restore',
                              extra_env={'LIBREECHO_DATA_ROOT': spelling})
                    for relative, value in self.files.items():
                        self.assertEqual((dotted / relative).read_text(), value,
                                         'state changed: ' + relative)
        finally:
            sibling.unlink()
            dotted.rename(DATA)
        with tarfile.open(ARCHIVE, 'r:gz') as archive:
            payload = b''
            for member in archive.getmembers():
                handle = archive.extractfile(member) if member.isfile() else None
                if handle is not None:
                    payload += handle.read()
        self.assertNotIn(b'prefix-confusion', payload,
                         'a sibling root was confused with the configured one')

    def test_service_restart_failure_is_not_success(self):
        self.call('create')
        Path('/run/start-fail-libreecho-web').touch()
        self.call('restore', success=False)


    def test_legacy_manifest_restore_applies_the_exclusion_contract(self):
        # A version-1 manifest names no exclusions and was written by a tool
        # that captured one-shot requests, transaction files, and stale `.bak`
        # copies. The exclusions belong to the contract, not to the manifest,
        # so restoring that archive must drop them rather than install them.
        one_shot = 'legacy-one-shot-' + secrets.token_hex(8)
        transaction = 'legacy-transaction-' + secrets.token_hex(8)
        stale = 'legacy-stale-copy-' + secrets.token_hex(8)
        committed = {
            'config/web-config.json': '{"hostname":"legacy-user-configured"}\n',
            'config/users': 'legacy-account\n',
            'config/agent.json': '{"provider":"legacy"}\n',
            'config/nested/setting': 'legacy-nested\n',
            'secrets/openai-codex.json': 'legacy-secret\n',
        }
        excluded = {
            'config/wake-dump.raw': one_shot,
            'config/wake-dump-seconds': one_shot,
            # A directory standing at an excluded path is dropped with it.
            'config/vendor-import-force-next-boot/inner': one_shot,
            'config/web-config.json.tmp': transaction,
            'secrets/provider.new': transaction,
            'config/users.bak': stale,
            'secrets/openai-codex.json.bak': stale,
        }

        def legacy(stage):
            for relative, value in dict(committed, **excluded).items():
                write(stage / 'persistent' / relative, value)
            write(stage / 'manifest.json',
                  '{"version":1,"components":["config","secrets"]}\n')

        archive = self.stage_archive('legacy-v1.tar.gz', legacy)
        listed = self.call('list', path=archive)
        self.assertIn('"version":1', listed.stdout)
        self.assertIn('persistent/config/wake-dump-seconds', listed.stdout)
        self.assertIn(EXCLUSION_NOTICE, listed.stdout)
        restored = self.call('restore', path=archive)
        self.assertIn(EXCLUSION_NOTICE, restored.stdout)
        for value in [one_shot, transaction, stale]:
            self.assertNotIn(value, restored.stdout + restored.stderr)
        for relative, value in committed.items():
            path = DATA / relative
            self.assertTrue(path.read_text() == value,
                            'restored bytes differ: ' + relative)
        expected = {'config', 'secrets'} | set(committed)
        for relative in committed:
            parent = Path(relative).parent
            while str(parent) != '.':
                expected.add(str(parent))
                parent = parent.parent
        installed = set()
        for root in [DATA / 'config', DATA / 'secrets']:
            for path in [root] + list(root.rglob('*')):
                installed.add(str(path.relative_to(DATA)))
        self.assertEqual(installed, expected,
                         'a file the contract never restores was installed')
        for suffix in ['wake-dump.raw', 'wake-dump-seconds',
                       'vendor-import-force-next-boot']:
            self.assertFalse((DATA / 'config' / suffix).exists(), suffix)
        for suffix in ['*.bak', '*.tmp', '*.new']:
            self.assertEqual([str(path.relative_to(DATA)) for root in
                              [DATA / 'config', DATA / 'secrets']
                              for path in root.rglob(suffix)], [])

    def test_symlinked_or_non_regular_manifest_is_refused_before_reading(self):
        # `list` reads the manifest with root privileges, so a manifest that is
        # a link would print whatever it points at, and a manifest that is not
        # a regular file is not a backup at all. Both are refused before the
        # manifest is read or listed, and live state is left alone.
        disclosure = 'root-readable-' + secrets.token_hex(16)
        write('/etc/manifest-target', disclosure + '\n')
        Path('/etc/manifest-target').chmod(0o600)

        def linked(stage):
            self.write_basic_trees(stage)
            os.symlink('/etc/manifest-target', stage / 'manifest.json')

        def dangling(stage):
            self.write_basic_trees(stage)
            os.symlink('/etc/absent-manifest-target', stage / 'manifest.json')

        def directory(stage):
            self.write_basic_trees(stage)
            write(stage / 'manifest.json/inner', '{"version":2}\n')

        def fifo(stage):
            self.write_basic_trees(stage)
            os.mkfifo(stage / 'manifest.json')

        cases = [('linked', linked, 'symbolic link'),
                 ('dangling', dangling, 'symbolic link'),
                 ('directory', directory, 'not a regular file'),
                 ('fifo', fifo, 'not a regular file')]
        for name, builder, reason in cases:
            with self.subTest(manifest=name):
                archive = self.stage_archive('manifest-' + name + '.tar.gz',
                                             builder)
                for action in ['list', 'restore']:
                    with self.subTest(manifest=name, action=action):
                        result = self.call(action, path=archive, success=False)
                        self.assertIn(reason, result.stderr)
                        self.assertNotIn(disclosure,
                                         result.stdout + result.stderr)
                        self.assertNotIn('Contents:', result.stdout)
                for relative, value in self.files.items():
                    self.assertEqual((DATA / relative).read_text(), value,
                                     'live state changed: ' + relative)
        self.assertEqual(self.actions('stop'), [],
                         'services were stopped for a refused archive')

    def test_stale_pre_update_copies_are_never_captured(self):
        # `config_write_atomic` keeps the previous bytes of a file behind a
        # hard-linked `<path>.bak`, so a `.bak` of web-config.json or users is
        # a durable stale copy of account state or credentials, not committed
        # state. A name that merely ends in another suffix is committed state.
        stale = 'stale-copy-' + secrets.token_hex(16)
        for relative in ['config/web-config.json.bak', 'config/users.bak',
                         'secrets/openai-codex.json.bak',
                         'secrets/nested/agent.json.bak']:
            write(DATA / relative, stale + '\n')
        keep = {'config/settings.bak.keep': 'kept-almost-bak\n',
                'config/nested/firmware.bak.txt': 'kept-almost-bak\n'}
        for relative, value in keep.items():
            write(DATA / relative, value)
        created = self.call('create')
        self.assertNotIn(stale, created.stdout + created.stderr)
        with tarfile.open(ARCHIVE, 'r:gz') as archive:
            names = [member.name for member in archive.getmembers()]
            payload = b''
            for member in archive.getmembers():
                handle = archive.extractfile(member) if member.isfile() else None
                if handle is not None:
                    payload += handle.read()
            manifest_file = archive.extractfile('manifest.json')
            assert manifest_file is not None
            excluded = json.load(manifest_file)['excluded']
        self.assertEqual([name for name in names if name.endswith('.bak')], [],
                         'a stale pre-update copy was archived')
        self.assertNotIn(stale.encode(), payload)
        for relative in keep:
            self.assertIn('persistent/' + relative, names)
        self.assertIn('transaction-files:config,secrets:*.tmp,*.new,*.bak',
                      excluded)
        self.assertNotIn('transaction-files:config,secrets:*.tmp,*.new',
                         excluded)

    def test_archive_member_that_escapes_the_root_is_refused(self):
        # A member name that leaves the extraction root - an absolute path, or
        # one with a `..` component - is not guaranteed to land inside the
        # staging directory when it is extracted, so an archive that names one
        # is refused before extraction, by both operations.
        def escape(stage):
            self.write_basic_trees(stage)
            write(stage / 'manifest.json', '{"version":2}\n')

        archive = self.stage_archive('escape.tar.gz', escape)
        hostile = Path('/out/escape-backup.tar.gz')
        with tarfile.open(archive, 'r:gz') as source, \
                tarfile.open(hostile, 'w:gz') as target:
            for member in source.getmembers():
                handle = source.extractfile(member) if member.isfile() else None
                target.addfile(member, handle)
            for name in ['../escape-outside.txt', '/tmp/escape-absolute.txt']:
                info = tarfile.TarInfo(name)
                body = b'escaped-member\n'
                info.size = len(body)
                target.addfile(info, io.BytesIO(body))
        with tarfile.open(hostile, 'r:gz') as handle:
            self.assertIn('../escape-outside.txt',
                          [member.name for member in handle.getmembers()])
        for action in ['list', 'restore']:
            with self.subTest(action=action):
                result = self.call(action, path=hostile, success=False)
                self.assertIn('escapes the archive root', result.stderr)
        self.assertFalse(Path('/tmp/escape-outside.txt').exists(),
                         'an escaping member was extracted')
        self.assertFalse(Path('/tmp/escape-absolute.txt').exists(),
                         'an escaping member was extracted')
        self.assertEqual(self.actions('stop'), [],
                         'services were stopped for a refused archive')
        for relative, value in self.files.items():
            self.assertEqual((DATA / relative).read_text(), value,
                             'live state changed: ' + relative)


if __name__ == '__main__':
    unittest.main(verbosity=2)
