"""The main executable must be signed with its bundle after all nested code."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'package_signing', Path(__file__).resolve().parents[2] / 'scripts/packaging/package.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class MacSigningTests(unittest.TestCase):
    def test_nested_code_is_signed_before_the_bundle_in_any_directory_order(self):
        for main_first in [True, False]:
            with self.subTest(main_first=main_first), tempfile.TemporaryDirectory() as temporary:
                app = Path(temporary) / 'OpenXMB.app'
                main = app / 'Contents/MacOS/OpenXMB'
                helpers = {app / 'Contents/MacOS/pdftoppm', app / 'Contents/Frameworks/libfixture.dylib'}
                for path in ([main, *sorted(helpers)] if main_first else [*sorted(helpers), main]):
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(b'\xcf\xfa\xed\xfe' + b'fixture')
                signed = set()
                verified = []

                def codesign(*arguments):
                    target = Path(arguments[-1])
                    if '--verify' in arguments:
                        self.assertEqual(signed, helpers | {app})
                        verified.append(target)
                    elif target == app:
                        self.assertEqual(signed, helpers, 'Nested helpers must be signed before the app')
                        signed.add(app)
                    else:
                        self.assertNotEqual(target, main, 'Sign the main executable via its bundle')
                        signed.add(target)

                with patch.object(package, 'dependencies', return_value=[]), \
                     patch.object(package, 'run', side_effect=codesign):
                    package.sign_and_verify(app)
                self.assertEqual(verified, [app])

    def test_external_runtime_dependency_blocks_bundle_signing(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / 'OpenXMB.app'
            main = app / 'Contents/MacOS/OpenXMB'
            main.parent.mkdir(parents=True)
            main.write_bytes(b'\xcf\xfa\xed\xfe' + b'fixture')
            with patch.object(package, 'dependencies', return_value=['/usr/local/lib/unbundled.dylib']), \
                 patch.object(package, 'run') as codesign:
                with self.assertRaisesRegex(RuntimeError, 'Unbundled runtime dependency'):
                    package.sign_and_verify(app)
                self.assertFalse(any(call.args[-1] == app for call in codesign.call_args_list))


if __name__ == '__main__':
    unittest.main()
