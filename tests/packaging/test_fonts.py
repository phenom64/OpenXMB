"""Release font provisioning works on clean and previously used runners."""
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('ci_fonts', ROOT / 'scripts/ci/fonts.py')
fonts = importlib.util.module_from_spec(spec)


class FontProvisioningTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec.loader.exec_module(fonts)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cache = self.root / 'cache'
        self.destinations = [self.root / 'build/shell/fonts', self.root / 'stage/share/shell/fonts']
        self.payload = b'a small stand-in for a font'
        self.item = ('font.ttf', 'https://example.invalid/font.ttf', hashlib.sha256(self.payload).hexdigest())

    def provision(self):
        fonts.provision(self.cache, self.destinations, [self.item])

    def test_downloads_and_stages_verified_file_and_reuses_cache(self):
        with patch.object(fonts.urllib.request, 'urlopen', return_value=io.BytesIO(self.payload)) as request:
            self.provision()
            self.provision()
        self.assertEqual(request.call_count, 1)
        self.assertEqual((self.cache / 'font.ttf').read_bytes(), self.payload)
        for dest in self.destinations:
            self.assertEqual((dest / 'font.ttf').read_bytes(), self.payload)

    def test_tampered_cache_is_replaced_and_staged_copies_repaired(self):
        self.cache.mkdir(parents=True)
        (self.cache / 'font.ttf').write_bytes(b'tampered')
        with patch.object(fonts.urllib.request, 'urlopen', return_value=io.BytesIO(self.payload)):
            self.provision()
        self.assertEqual((self.cache / 'font.ttf').read_bytes(), self.payload)
        (self.destinations[0] / 'font.ttf').write_bytes(b'tampered')
        with patch.object(fonts.urllib.request, 'urlopen', side_effect=AssertionError('unexpected download')):
            self.provision()
        self.assertEqual((self.destinations[0] / 'font.ttf').read_bytes(), self.payload)

    def test_checksum_mismatch_fails_without_publishing_download(self):
        with patch.object(fonts.urllib.request, 'urlopen', return_value=io.BytesIO(b'wrong')):
            with self.assertRaisesRegex(RuntimeError, 'SHA-256'):
                self.provision()
        self.assertFalse((self.cache / 'font.ttf').exists())
        self.assertFalse((self.destinations[0] / 'font.ttf').exists())

    def test_network_failure_is_bounded_and_leaves_no_cached_file(self):
        with patch.object(fonts.urllib.request, 'urlopen', side_effect=OSError('offline')) as request, \
             patch.object(fonts.time, 'sleep'):
            with self.assertRaisesRegex(RuntimeError, 'offline'):
                self.provision()
        self.assertEqual(request.call_count, 3)
        self.assertFalse((self.cache / 'font.ttf').exists())

    def test_retries_transient_network_failure(self):
        with patch.object(fonts.urllib.request, 'urlopen', side_effect=[OSError('transient'), io.BytesIO(self.payload)]) as request, \
             patch.object(fonts.time, 'sleep'):
            self.provision()
        self.assertEqual(request.call_count, 2)
        self.assertEqual((self.destinations[0] / 'font.ttf').read_bytes(), self.payload)

    def test_main_uses_platform_stage_and_build_tree_paths(self):
        for system, installed_shell in [('Windows', 'shell'), ('Linux', 'share/shell'), ('Darwin', 'share/shell')]:
            with self.subTest(system=system):
                build_dir = (self.root / system / 'build').resolve()
                stage = (self.root / system / 'stage').resolve()
                args = ['fonts.py', '--build-dir', str(build_dir), '--stage', str(stage)]
                def fake_provision(cache, destinations):
                    for directory in destinations:
                        directory.mkdir(parents=True, exist_ok=True)
                    self.assertEqual(cache, build_dir / 'private/font-cache')
                    self.assertEqual(destinations, [build_dir / 'shell/fonts', build_dir / 'share/shell/fonts',
                                                    stage / installed_shell / 'fonts'])
                with patch.object(sys, 'argv', args), patch.object(fonts.platform, 'system', return_value=system), \
                     patch.object(fonts, 'provision', side_effect=fake_provision):
                    fonts.main()
                self.assertIn('Adobe', (stage / installed_shell / 'fonts/COPYRIGHT-NotoCJK.txt').read_text())
                self.assertIn('Google', (stage / installed_shell / 'fonts/COPYRIGHT-Noto.txt').read_text())

    def test_build_driver_provisions_before_configure_and_after_install(self):
        driver_spec = importlib.util.spec_from_file_location('font_build_driver', ROOT / 'scripts/ci/build.py')
        driver = importlib.util.module_from_spec(driver_spec)
        driver_spec.loader.exec_module(driver)
        commands = []
        args = ['build.py', '--arch', 'x86_64', '--build-dir', str(self.root / 'build'),
                '--stage', str(self.root / 'stage')]
        with patch.object(sys, 'argv', args), patch.dict(os.environ, {}, clear=True), \
             patch.object(driver.platform, 'system', return_value='Linux'), \
             patch.object(driver.shutil, 'which', return_value='/tools/gettext'), \
             patch.object(driver, 'validate_media_tools'), \
             patch.object(driver, 'run', side_effect=lambda *args: commands.append([str(a) for a in args])), \
             patch.object(driver.subprocess, 'run', side_effect=[
                 subprocess.CompletedProcess([], 0), subprocess.CompletedProcess([], 0)]), \
             patch.object(driver.subprocess, 'check_output', return_value='sha\n'):
            driver.main()
        self.assertTrue(commands[0][1].endswith('scripts\\ci\\fonts.py') or commands[0][1].endswith('scripts/ci/fonts.py'))
        self.assertIn('-S', commands[1])
        self.assertEqual(commands[2][:2], ['cmake', '--install'])
        self.assertIn('--stage', commands[3])


if __name__ == '__main__':
    unittest.main()
