"""A native startup failure must leave its diagnostic output available to CI."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'package_smoke', Path(__file__).resolve().parents[2] / 'scripts/packaging/smoke.py')
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


class StartupDiagnosticsTests(unittest.TestCase):
    def test_failed_process_preserves_stdout_stderr_and_exit_status(self):
        with tempfile.TemporaryDirectory(prefix='OpenXMB startup test ') as directory:
            log = Path(directory) / 'startup.log'
            command = [sys.executable, '-c',
                       'import sys; print("entered main", flush=True); '
                       'print("startup exception", file=sys.stderr, flush=True); sys.exit(23)']
            with self.assertRaises(subprocess.CalledProcessError) as failure:
                smoke.checked_output(command, os.environ.copy(), directory, log)
            self.assertEqual(failure.exception.returncode, 23)
            self.assertIn('entered main', log.read_text())
            self.assertIn('startup exception', failure.exception.output)

    def test_successful_process_returns_captured_version_output(self):
        with tempfile.TemporaryDirectory(prefix='OpenXMB startup test ') as directory:
            output = smoke.checked_output([sys.executable, '-c', 'print("version fixture")'],
                                          os.environ.copy(), directory, Path(directory) / 'version.log')
            self.assertEqual(output.strip(), 'version fixture')


if __name__ == '__main__':
    unittest.main()
