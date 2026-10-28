"""The release must never publish from a branch build or an unrelated merge."""
import importlib.util
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('release_gate', Path(__file__).resolve().parents[2] / 'scripts/ci/release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.names = ['openxmb_0.3.0~beta.3_amd64.deb',
                      'OpenXMB-0.3.0-beta.3-windows-x86_64.zip',
                      'OpenXMB-0.3.0-beta.3-macos-universal.dmg',
                      'OpenXMB-0.3.0-beta.3-linux-x86_64.tar.gz']
        for name in self.names:
            contents = (name + '\n').encode() * 100
            (self.directory / name).write_bytes(contents)
            (self.directory / (name + '.sha256')).write_text(
                hashlib.sha256(contents).hexdigest() + '  ' + name + '\n')

    def verify(self):
        return release.verify_assets(self.directory, '0.3.0-beta.3')

    def test_complete_artifacts_produce_release_checksums(self):
        paths = self.verify()
        self.assertEqual({path.name for path in paths},
                         {*(name.replace('~', '.') for name in self.names), 'SHA256SUMS'})
        self.assertEqual(len((self.directory / 'SHA256SUMS').read_text().splitlines()), 4)

    def test_published_filenames_match_manifest_and_preserve_package_bytes(self):
        paths = self.verify()
        manifest = (self.directory / 'SHA256SUMS').read_text().splitlines()
        for path, line in zip(paths[:-1], manifest):
            digest, name = line.split()
            self.assertEqual(name, path.name)
            self.assertNotIn('~', name)
            self.assertEqual(digest, hashlib.sha256(path.read_bytes()).hexdigest())
        self.assertEqual(paths[0].read_bytes(), (self.directory / self.names[0]).read_bytes())

    def test_missing_platform_blocks_release(self):
        (self.directory / self.names[1]).unlink()
        with self.assertRaisesRegex(RuntimeError, 'Missing release artifact'):
            self.verify()

    def test_missing_portable_linux_blocks_release(self):
        (self.directory / self.names[3]).unlink()
        with self.assertRaisesRegex(RuntimeError, 'Missing release artifact'):
            self.verify()

    def test_changed_package_blocks_release(self):
        with (self.directory / self.names[2]).open('ab') as handle:
            handle.write(b'changed')
        with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
            self.verify()

    def test_empty_package_blocks_release(self):
        (self.directory / self.names[0]).write_bytes(b'')
        with self.assertRaisesRegex(RuntimeError, 'Missing release artifact'):
            self.verify()


class ReleaseRecoveryTests(unittest.TestCase):
    def test_existing_correct_tag_is_preserved(self):
        with patch.object(release,'api',return_value={'object':{'sha':'approved-sha'}}), patch.object(release.subprocess,'run') as run:
            release.reserve_tag('phenom64/OpenXMB','approved-sha','v0.3.0-beta.3')
            run.assert_not_called()

    def test_conflicting_tag_is_never_moved(self):
        with patch.object(release,'api',return_value={'object':{'sha':'other-sha'}}), patch.object(release.subprocess,'run') as run:
            with self.assertRaises(RuntimeError):release.reserve_tag('phenom64/OpenXMB','approved-sha','v0.3.0-beta.3')
            run.assert_not_called()

    def test_missing_tag_is_reserved_explicitly(self):
        with patch.object(release,'api',return_value=None), patch.object(release.subprocess,'run') as run:
            release.reserve_tag('phenom64/OpenXMB','approved-sha','v0.3.0-beta.3')
            self.assertIn('sha=approved-sha',run.call_args.args[0])


class DraftLookupTests(unittest.TestCase):
    def test_draft_is_found_when_tag_endpoint_omits_it(self):
        draft={'id':123,'draft':True,'tag_name':'v0.3.0-beta.3'}
        with patch.object(release,'api',side_effect=[None,[draft]]):
            self.assertEqual(release.find_release('phenom64/OpenXMB','v0.3.0-beta.3'),draft)


if __name__ == '__main__':
    unittest.main()
