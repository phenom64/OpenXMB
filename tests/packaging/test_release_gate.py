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


class ReleaseGateTests(unittest.TestCase):
    def setUp(self):
        self.pr = {'number': 8, 'merged_at': '2026-10-27T12:00:00Z', 'merge_commit_sha': 'merge-sha',
                   'base': {'ref': 'master'}, 'head': {'ref': 'RSX-26', 'repo': {'full_name': 'phenom64/OpenXMB'}}}

    def check_gate(self, event='push', ref='refs/heads/master', sha='merge-sha'):
        return release.eligible(event, ref, sha, 'phenom64/OpenXMB', [self.pr])

    def test_merged_release_pr(self):
        self.assertTrue(self.check_gate())

    def test_branch_push_cannot_publish(self):
        self.assertFalse(self.check_gate(ref='refs/heads/RSX-26'))

    def test_pull_request_cannot_publish(self):
        self.assertFalse(self.check_gate(event='pull_request'))

    def test_dispatch_cannot_publish(self):
        self.assertFalse(self.check_gate(event='workflow_dispatch'))

    def test_unmerged_pr_cannot_publish(self):
        self.pr['merged_at'] = None
        self.assertFalse(self.check_gate())

    def test_unrelated_commit_cannot_publish(self):
        self.assertFalse(self.check_gate(sha='other-sha'))

    def test_different_pr_cannot_publish(self):
        self.pr['number'] = 9
        self.assertFalse(self.check_gate())

    def test_fork_cannot_publish(self):
        self.pr['head']['repo']['full_name'] = 'someone/OpenXMB'
        self.assertFalse(self.check_gate())

    def test_master_context_checks_the_approved_pr_directly(self):
        environment = {'GITHUB_REPOSITORY': 'phenom64/OpenXMB', 'GITHUB_SHA': 'merge-sha',
                       'GITHUB_EVENT_NAME': 'push', 'GITHUB_REF': 'refs/heads/master'}
        with patch.dict(release.os.environ, environment), patch.object(release, 'api', return_value=self.pr) as api:
            self.assertEqual(release.context(), ('phenom64/OpenXMB', 'merge-sha', True))
            api.assert_called_once_with('repos/phenom64/OpenXMB/pulls/8', missing_ok=True)

    def test_pr_context_does_not_query_publication_state(self):
        environment = {'GITHUB_REPOSITORY': 'phenom64/OpenXMB', 'GITHUB_SHA': 'test-merge-sha',
                       'GITHUB_EVENT_NAME': 'pull_request', 'GITHUB_REF': 'refs/pull/8/merge'}
        with patch.dict(release.os.environ, environment), patch.object(release, 'api') as api:
            self.assertFalse(release.context()[2])
            api.assert_not_called()

    def test_missing_release_pr_cannot_publish(self):
        environment = {'GITHUB_REPOSITORY': 'someone/OpenXMB', 'GITHUB_SHA': 'fork-sha',
                       'GITHUB_EVENT_NAME': 'push', 'GITHUB_REF': 'refs/heads/master'}
        with patch.dict(release.os.environ, environment), patch.object(release, 'api', return_value=None):
            self.assertFalse(release.context()[2])


class ReleaseArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.names = ['openxmb_0.2.0~beta.2_amd64.deb',
                      'OpenXMB-0.2.0-beta.2-windows-x86_64.zip',
                      'OpenXMB-0.2.0-beta.2-macos-universal.dmg']
        for name in self.names:
            contents = (name + '\n').encode() * 100
            (self.directory / name).write_bytes(contents)
            (self.directory / (name + '.sha256')).write_text(
                hashlib.sha256(contents).hexdigest() + '  ' + name + '\n')

    def verify(self):
        return release.verify_assets(self.directory, '0.2.0-beta.2')

    def test_complete_artifacts_produce_release_checksums(self):
        paths = self.verify()
        self.assertEqual({path.name for path in paths}, {*self.names, 'SHA256SUMS'})
        self.assertEqual(len((self.directory / 'SHA256SUMS').read_text().splitlines()), 3)

    def test_missing_platform_blocks_release(self):
        (self.directory / self.names[1]).unlink()
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


if __name__ == '__main__':
    unittest.main()
