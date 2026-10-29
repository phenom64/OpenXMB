"""Read-only contract checks; no owned GUI fixtures or activation calls."""
import ctypes
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

MODULE = Path(__file__).with_name('guide_focus_diagnostics.py')


class FocusDiagnosticsTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(MODULE.is_file(), 'focus collector is not implemented')
        spec = importlib.util.spec_from_file_location('guide_focus_diagnostics', MODULE)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)

    def test_canonical_handles_reject_sign_whitespace_and_overflow(self):
        for text in ('42', '0x2a'):
            self.assertEqual(self.module.parse_hwnd(text), 42)
        for text in ('0', '-1', '+1', ' 42', '042', '0X2A', '0x', str(1 << (8 * ctypes.sizeof(ctypes.c_void_p)))):
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.module.parse_hwnd(text)

    def test_exclusive_writer_preserves_existing_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'snapshot.json'
            path.write_text('existing', encoding='utf-8')
            with self.assertRaises(FileExistsError):
                self.module.write_snapshot(path, {'schema_version': 1})
            self.assertEqual(path.read_text(encoding='utf-8'), 'existing')

    @unittest.skipUnless(os.name == 'nt', 'Windows APIs unavailable')
    def test_invalid_identity_cli_leaves_no_output(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'invalid.json'
            result = subprocess.run([sys.executable, str(MODULE), '--shell-pid', str(os.getpid()),
                                     '--game-pid', str(os.getpid()), '--game-hwnd', '1', '--output', str(output)],
                                    capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('window', result.stderr.lower())
            self.assertFalse(output.exists())

    @unittest.skipUnless(os.name == 'nt', 'Windows APIs unavailable')
    def test_process_handle_and_gui_structures_keep_pointer_width(self):
        api = self.module._WinAPI()
        self.assertEqual(ctypes.sizeof(api.kernel.OpenProcess.restype), ctypes.sizeof(ctypes.c_void_p))
        self.assertEqual(ctypes.sizeof(api.user.GetForegroundWindow.restype), ctypes.sizeof(ctypes.c_void_p))
        self.assertEqual(ctypes.sizeof(self.module.GUIThreadInfo), 72 if ctypes.sizeof(ctypes.c_void_p) == 8 else 48)
        with self.assertRaises(self.module.SnapshotError):
            self.module.capture_snapshot(os.getpid(), os.getpid(), 1)

    @unittest.skipUnless(os.name == 'nt', 'Windows APIs unavailable')
    def test_bool_failure_with_no_extended_error_stays_unavailable(self):
        api = self.module._WinAPI()
        # WinAPI failure injection is needed to exercise the documented BOOL
        # contract; a zero last-error value must not turn FALSE into success.
        api.user.GetGUIThreadInfo = lambda *args: 0
        ctypes.set_last_error(0)
        result = api.gui_thread(1)
        self.assertFalse(result['available'])
        self.assertEqual(result['winerror'], 0)
        self.assertIn('without an extended error', result['detail'])

    @unittest.skipUnless(os.name == 'nt', 'Windows APIs unavailable')
    def test_invalid_process_rejected(self):
        with self.assertRaisesRegex(self.module.SnapshotError, 'process 4294967295'):
            self.module.capture_snapshot(0xffffffff, os.getpid(), 1)

    @unittest.skipUnless(os.name == 'nt', 'Windows APIs unavailable')
    def test_window_owned_by_different_process_rejected(self):
        api = self.module._WinAPI()
        hwnd = api.user.GetForegroundWindow()
        if not hwnd or api.owner(hwnd)['pid'] == os.getpid():
            self.skipTest('No distinct foreground process available')
        with self.assertRaisesRegex(self.module.SnapshotError, 'belongs to PID'):
            self.module.capture_snapshot(os.getpid(), os.getpid(), hwnd)

    @unittest.skipUnless(os.name == 'nt', 'Windows APIs unavailable')
    def test_creation_identity_change_is_rejected(self):
        api = self.module._WinAPI()
        hwnd = api.user.GetForegroundWindow()
        if not hwnd:
            self.skipTest('No foreground window available')
        game_pid = api.owner(hwnd)['pid']
        original = self.module._WinAPI.open_process
        shell_opens = 0

        def simulate_reused_pid(instance, pid):
            nonlocal shell_opens
            handle, identity = original(instance, pid)
            if pid == os.getpid():
                shell_opens += 1
                if shell_opens > 1:
                    identity['creation_filetime_100ns'] = str(int(identity['creation_filetime_100ns']) + 1)
            return handle, identity

        with patch.object(self.module._WinAPI, 'open_process', simulate_reused_pid):
            with self.assertRaisesRegex(self.module.SnapshotError, 'identity changed'):
                self.module.capture_snapshot(os.getpid(), game_pid, hwnd)

    @unittest.skipUnless(os.name == 'nt', 'Windows APIs unavailable')
    def test_current_foreground_can_be_observed_without_activation(self):
        api = self.module._WinAPI()
        hwnd = api.user.GetForegroundWindow()
        if not hwnd:
            self.skipTest('No foreground window on this Windows session')
        identity = api.owner(hwnd)
        if not identity['pid']:
            self.skipTest('Foreground owner unavailable')
        try:
            result = self.module.capture_snapshot(os.getpid(), identity['pid'], hwnd)
        except self.module.SnapshotError as error:
            self.skipTest(f'Foreground changed or process inaccessible: {error}')
        self.assertEqual(result['target']['game_hwnd'], hex(hwnd))
        self.assertEqual(result['processes']['shell']['pid'], os.getpid())
        self.assertTrue(result['game_window']['ownership_stable'])
        self.assertIn('last_input', result)
        self.assertNotIn('title', str(result.keys()))


if __name__ == '__main__':
    unittest.main()
