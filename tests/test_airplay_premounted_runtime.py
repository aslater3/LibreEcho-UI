"""Execute shipped runtime helpers with no host mounts or hardware accesses."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = Path(os.environ.get('SCRIPT', ROOT / 'init/libreecho-airplayd.init'))

class RuntimeMounts(unittest.TestCase):
    def run_case(self, mounted=(), fail=''):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            runtime = root / 'runtime'
            (runtime / 'etc/avahi/services').mkdir(parents=True)
            payload = root / 'payload.squashfs'
            payload.touch()
            state = root / 'mounts'
            state.write_text(''.join(f'{runtime}{suffix}\n' for suffix in mounted))
            log = root / 'log'
            log.touch()
            led_log = root / 'led-calls'
            led_log.touch()
            bindir = root / 'bin'
            bindir.mkdir()
            # All mount operations are simulated, but successive attempts see
            # the actual state left by the preceding successful operations.
            scripts = {
                'mount': '''#!/bin/sh
for target do :; done
printf '%s\\n' "$*" >> "$MOUNT_LOG"
[ "$target" != "$FAIL_TARGET" ] || exit 1
printf '%s\\n' "$target" >> "$MOUNT_STATE"
''',
                'grep': '''#!/bin/sh
# Production helpers use precisely grep -q " destination " /proc/mounts.
pattern=$2
[ "$3" = /proc/mounts ] || exit 2
pattern=${pattern# }; pattern=${pattern% }
/bin/grep -Fxq "$pattern" "$MOUNT_STATE"
''',
                'mkdir': '''#!/bin/sh
# Only the literal shared audio root is external to this fixture.
for arg do
    case "$arg" in -p|/run/libreecho-audio) continue ;; esac
    /bin/mkdir -p -- "$arg" || exit 1
done
'''
            }
            for name, body in scripts.items():
                p=bindir/name;p.write_text(body);p.chmod(0o755)
            source=SCRIPT.read_text()
            names=['mount_support','create_support_mounts','mount_runtime','prepare_avahi_runtime']
            helpers=[]
            for name in names:
                found=re.search(r'^'+name+r'\(\) \{\n.*?^\}',source,re.M|re.S)
                if found:helpers.append(found.group())
            env=dict(os.environ,PATH=str(bindir)+':'+os.environ['PATH'],
                     RUNTIME_ROOT=str(runtime),PAYLOAD=str(payload),
                     MOUNT_LOG=str(log),MOUNT_STATE=str(state),
                     LED_LOG=str(led_log),
                     FAIL_TARGET=str(runtime)+fail if fail else '-',
                     AVAHI_CONFIG_SNAPSHOT=str(root/'avahi.conf'),
                     AVAHI_SERVICES_SOURCE=str(root/'absent'))
            # Record the optional LED bridge boundary separately from mounts.
            # The production mount_runtime must reach it on every successful
            # path, even when the transaction engine already mounted the root.
            program = '\n'.join(helpers) + '''
mount_led_socket() { printf '%s\\n' led-bridge >> "$LED_LOG"; }
mount_runtime
'''
            proc=subprocess.run(['sh','-c',program],env=env,capture_output=True,text=True,timeout=5)
            return proc.returncode,log.read_text(),str(runtime),led_log.read_text().splitlines()

    def test_bare_transaction_mount_gets_writable_support(self):
        rc,log,root,led=self.run_case([''])
        self.assertEqual(rc,0)
        for suffix in ['/dev','/proc','/sys','/run','/var','/dev/shm','/run/libreecho-audio']:
            self.assertIn(root+suffix+'\n',log)
        self.assertNotIn('-t squashfs',log)
        self.assertEqual(led, ['led-bridge'])

    def test_complete_runtime_is_idempotent(self):
        rc,log,_,led=self.run_case(['','/dev','/proc','/sys','/run','/var','/dev/shm','/run/libreecho-audio'])
        self.assertEqual(rc,0);self.assertEqual(log,'')
        self.assertEqual(led, ['led-bridge'])

    def test_partial_attempt_does_not_treat_run_as_completion(self):
        rc,log,root,led=self.run_case(['','/dev','/proc','/sys','/run'])
        self.assertEqual(rc,0)
        self.assertIn('libreecho-airplay-var '+root+'/var',log)
        self.assertNotIn('libreecho-airplay-run ',log)
        self.assertEqual(led, ['led-bridge'])

    def test_support_failure_is_propagated(self):
        for suffix in ['/dev','/proc','/sys','/run','/var','/dev/shm','/run/libreecho-audio']:
            with self.subTest(suffix=suffix):
                rc,_,_,led=self.run_case([''],suffix)
                self.assertNotEqual(rc,0)
                self.assertEqual(led, [])

    def test_fresh_runtime_mounts_payload_and_support(self):
        rc,log,_,led=self.run_case()
        self.assertEqual(rc,0);self.assertIn('-t squashfs',log)
        self.assertIn('libreecho-airplay-run ',log)
        self.assertEqual(led, ['led-bridge'])

if __name__=='__main__':unittest.main()
