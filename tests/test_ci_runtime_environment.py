#!/usr/bin/env python3
"""Pin Actions context availability and real runtime environment exports."""
import os
from pathlib import Path
import subprocess
import tempfile
import ast
import socket
import unittest

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / '.github/workflows/checks.yml'
MARKER = '      - name: Configure private test paths\n'


class RuntimeEnvironmentTests(unittest.TestCase):
    def test_runner_context_not_used_in_job_environment(self):
        text = WORKFLOW.read_text()
        job = text.split('  source_checks:\n', 1)[1]
        before_steps = job.split('    steps:\n', 1)[0]
        self.assertNotIn('${{ runner.', before_steps)

    def test_paths_initialized_before_first_consumer(self):
        text = WORKFLOW.read_text()
        self.assertIn(MARKER, text)
        self.assertLess(text.index(MARKER), text.index('      - name: Retain exact tracked verification inputs'))

    def test_ci_nested_socket_paths_fit_real_unix_limit(self):
        text = WORKFLOW.read_text()
        tail = text.split('"TMPDIR=$RUNNER_TEMP/', 1)[1].split('"', 1)[0]
        runner = (ROOT / 'tests/run_tests.sh').read_text()
        template = runner.split('SUITE_TMP=$(mktemp -d "${TMPDIR:-$PWD/build}/', 1)[1].split('"', 1)[0]
        suite = template.replace('XXXXXX', '123456')
        tree = ast.parse((ROOT / 'tests/test_networkd_health_integration.py').read_text())
        prefixes = [kw.value.value for node in ast.walk(tree)
                    if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                    and node.func.attr == 'TemporaryDirectory'
                    for kw in node.keywords if kw.arg == 'prefix' and isinstance(kw.value, ast.Constant)
                    and isinstance(kw.value.value, str)]
        self.assertTrue(prefixes)
        lengths = [len(('/home/runner/work/_temp/' + tail + '/' + suite + '/' + prefix + '12345678/network.sock').encode()) for prefix in prefixes]
        self.assertLessEqual(max(lengths), 107, lengths)
        # Exercise a real bind at the calculated worst-case CI path length.
        with tempfile.TemporaryDirectory(prefix='sb-') as directory:
            component = max(lengths) - len(directory.encode()) - len('/network.sock') - 1
            self.assertGreaterEqual(component, 1)
            parent = Path(directory) / ('x' * component)
            parent.mkdir()
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
                server.bind(str(parent / 'network.sock'))

    def test_initializer_exports_actual_paths(self):
        text = WORKFLOW.read_text()
        self.assertIn(MARKER, text)
        block = text.split(MARKER, 1)[1].split('      - name:', 1)[0]
        script = '\n'.join(line[10:] for line in block.split('        run: |\n', 1)[1].splitlines())
        with tempfile.TemporaryDirectory(prefix='ci-env-') as directory:
            root = Path(directory)
            target = root / 'github-env'
            env = dict(os.environ, RUNNER_TEMP=str(root), GITHUB_ENV=str(target))
            subprocess.run(['bash', '-e', '-u', '-o', 'pipefail', '-c', script], env=env, check=True, timeout=10)
            values = dict(line.split('=', 1) for line in target.read_text().splitlines())
            self.assertEqual(values['TMPDIR'], str(root / 't'))
            self.assertTrue(Path(values['TMPDIR']).is_dir())
            for key in ['ESPHOMED_TLS_PREFIX', 'ESPHOMED_TEST_TLS_PREFIX']:
                self.assertEqual(values[key], str(root / 'mbedtls'))
            self.assertEqual(values['ESPHOMED_AIO_PYTHON'], str(root / 'esphome-client/bin/python'))
            self.assertEqual(values['CPPFLAGS'], '-I' + str(root / 'mbedtls/include'))
            self.assertEqual(values['ESPHOMED_TLS_LIBS'], ' '.join(str(root / ('mbedtls/lib/' + name)) for name in ['libmbedtls.a', 'libmbedx509.a', 'libmbedcrypto.a']))


if __name__ == '__main__':
    unittest.main()
