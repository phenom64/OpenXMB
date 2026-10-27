"""Verify ZIP contents from an installed tree, independently of the host OS."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

spec = importlib.util.spec_from_file_location(
    'package', Path(__file__).resolve().parents[2] / 'scripts/packaging/package.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PortableZipTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='OpenXMB package test ')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.stage = self.root / 'stage'
        self.destination = self.root / 'packages'
        self.destination.mkdir()
        self.prefix = self.root / 'vcpkg/x64-windows'
        self.poppler = self.root / 'poppler'
        self.redist = self.root / 'redist/x64/Microsoft.VC143.CRT'
        for name in ['XMS.bin.exe', 'openxmb-retro.exe', 'config.json',
                     'shell/catalog/en.json', 'locales/en/LC_MESSAGES/shell.mo',
                     'include/development.h', 'share/cmake/internal.cmake', 'build-info.json']:
            self.write(self.stage / name)
        for name in ['bin/SDL2.dll', 'tools/ffmpeg/ffmpeg.exe',
                     'tools/ffmpeg/ffprobe.exe', 'share/sdl2/copyright']:
            self.write(self.prefix / name)
        for name in ['bin/pdftoppm.exe', 'bin/poppler.dll', 'share/poppler/cMap/data',
                     'etc/fonts/fonts.conf', 'include/development.h', 'lib/static.lib']:
            self.write(self.poppler / name)
        for name in ['msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll']:
            self.write(self.redist / name)
        environment = {'VCPKG_INSTALLED_DIR': str(self.prefix.parent),
                       'OPENXMB_POPPLER_BIN': str(self.poppler / 'bin'),
                       'VCToolsRedistDir': str(self.root / 'redist')}
        self.environment = patch.dict(os.environ, environment)
        self.environment.start()
        self.addCleanup(self.environment.stop)

    @staticmethod
    def write(path):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'packaging fixture\n')

    def test_relocated_layout_has_runtime_helpers_assets_and_no_build_files(self):
        artifact = package.windows(self.stage, self.destination)
        with zipfile.ZipFile(artifact) as archive:
            prefix = f'OpenXMB-{package.VERSION}-windows-x86_64/'
            names = {name.removeprefix(prefix) for name in archive.namelist()}
        required = {'OpenXMB.exe', 'openxmb-retro.exe', 'config.json', 'SDL2.dll',
                    'ffmpeg.exe', 'ffprobe.exe', 'shell/catalog/en.json',
                    'locales/en/LC_MESSAGES/shell.mo', 'tools/poppler/bin/pdftoppm.exe',
                    'tools/poppler/bin/poppler.dll', 'tools/poppler/share/poppler/cMap/data',
                    'tools/poppler/etc/fonts/fonts.conf', 'msvcp140.dll',
                    'vcruntime140.dll', 'vcruntime140_1.dll', 'licenses/sdl2.txt'}
        self.assertFalse(required - names, required - names)
        self.assertFalse(any('development.h' in name or 'internal.cmake' in name
                             or name.endswith('.lib') or name == 'build-info.json'
                             or name == 'XMS.bin.exe' for name in names))

    def test_incomplete_crt_blocks_portable_package(self):
        (self.redist / 'vcruntime140_1.dll').unlink()
        with self.assertRaisesRegex(RuntimeError, 'Missing portable MSVC runtime'):
            package.windows(self.stage, self.destination)
        self.assertFalse(list(self.destination.glob('*.zip')))


if __name__ == '__main__':
    unittest.main()
