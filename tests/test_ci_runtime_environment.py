#!/usr/bin/env python3
"""Pin Actions context availability and real runtime environment exports."""
import os
from pathlib import Path
import subprocess
import tempfile
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
            self.assertEqual(values['TMPDIR'], str(root / 'libreecho-tests'))
            self.assertTrue(Path(values['TMPDIR']).is_dir())
            for key in ['ESPHOMED_TLS_PREFIX', 'ESPHOMED_TEST_TLS_PREFIX']:
                self.assertEqual(values[key], str(root / 'mbedtls'))
            self.assertEqual(values['ESPHOMED_AIO_PYTHON'], str(root / 'esphome-client/bin/python'))
            self.assertEqual(values['CPPFLAGS'], '-I' + str(root / 'mbedtls/include'))
            self.assertEqual(values['ESPHOMED_TLS_LIBS'], ' '.join(str(root / ('mbedtls/lib/' + name)) for name in ['libmbedtls.a', 'libmbedx509.a', 'libmbedcrypto.a']))


if __name__ == '__main__':
    unittest.main()
