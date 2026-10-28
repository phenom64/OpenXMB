"""Keep Linux's generated gettext fallback in the installed DEB resources."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'message_locale_package', Path(__file__).resolve().parents[2] / 'scripts/packaging/package.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class MessageLocaleTests(unittest.TestCase):
    def test_shared_assets_keep_native_locale_and_notice(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage, output = root / 'stage', root / 'deb/usr/share'
            for name in ('shell', 'OpenXMB', 'locale'):
                (stage / 'share' / name).mkdir(parents=True)
            native = stage / 'share/.native-locales'
            (native / 'en_US.UTF-8/LC_MESSAGES').mkdir(parents=True)
            message = b'compiled LC_MESSAGES fixture'
            (native / 'en_US.UTF-8/LC_MESSAGES/SYS_LC_MESSAGES').write_bytes(message)
            (native / 'COPYRIGHT').write_text('Locale source copyright\n')
            package.shared_assets(stage, output)
            self.assertEqual((output / '.native-locales/en_US.UTF-8/LC_MESSAGES/SYS_LC_MESSAGES').read_bytes(), message)
            self.assertEqual((output / '.native-locales/COPYRIGHT').read_text(), 'Locale source copyright\n')


if __name__ == '__main__':
    unittest.main()
