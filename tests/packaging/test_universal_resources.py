"""Normalize generated artwork while retaining strict shared-resource checks."""
import importlib.util
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'universal_package', Path(__file__).resolve().parents[2] / 'scripts/packaging/package.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class UniversalResourceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.archives = []
        for arch in ['x86_64', 'arm64']:
            app = self.root / arch / 'OpenXMB.app'
            (app / 'Contents/MacOS').mkdir(parents=True)
            (app / 'Contents/Resources').mkdir()
            (app / 'Contents/MacOS/OpenXMB').write_bytes(b'\xcf\xfa\xed\xfe' + arch.encode())
            (app / 'Contents/Resources/OpenXMB.icns').write_bytes(('encoded-' + arch).encode())
            (app / 'Contents/Resources/config.json').write_text('{"same": true}')
            self.archives.append(self.root / (arch + '.tar.gz'))

    def assemble(self, mismatch=False):
        if mismatch:
            (self.root / 'arm64/OpenXMB.app/Contents/Resources/config.json').write_text('{"different": true}')
        for arch, archive in zip(['x86_64', 'arm64'], self.archives):
            with tarfile.open(archive, 'w:gz') as handle:
                handle.add(self.root / arch / 'OpenXMB.app', arcname='OpenXMB.app')
        destination = self.root / 'universal'
        destination.mkdir()

        def run(*args):
            if args[0] == 'lipo' and '-create' in args:
                Path(args[-1]).write_bytes(b'\xca\xfe\xba\xbe' + b'universal fixture')

        def sign(app):
            self.assertEqual((app / 'Contents/Resources/OpenXMB.icns').read_bytes(), b'normalized source icon')

        with patch.object(package, 'output', return_value='x86_64'), \
             patch.object(package, 'run', side_effect=run), \
             patch.object(package, 'sign_and_verify', side_effect=sign), \
             patch.object(package, 'write_mac_icon', side_effect=lambda p: p.write_bytes(b'normalized source icon')) as icon:
            package.universal(*self.archives, self.root / 'wave.png', destination)
            self.assertEqual(icon.call_count, 1)

    def test_different_icon_encodings_are_regenerated_before_signing(self):
        self.assemble()

    def test_other_resource_differences_still_block_the_universal_package(self):
        with self.assertRaisesRegex(RuntimeError, 'Architecture resources differ:.*config.json'):
            self.assemble(mismatch=True)


if __name__ == '__main__':
    unittest.main()
