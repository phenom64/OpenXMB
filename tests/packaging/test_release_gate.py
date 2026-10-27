"""The release must never publish from a branch build or an unrelated merge."""
import importlib.util
from pathlib import Path
import unittest

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


if __name__ == '__main__':
    unittest.main()
