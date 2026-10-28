"""Manual publication must use the exact approved, validated master commit."""
import importlib.util
import os
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('release_approval', ROOT / 'scripts/ci/release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
SHA = 'a' * 40


class ApprovedReleaseTests(unittest.TestCase):
    def validate(self, approved=SHA, head=SHA, master=SHA, workflow_sha=SHA, event='workflow_dispatch', failed=None):
        env = {'GITHUB_REPOSITORY': 'example/OpenXMB', 'GITHUB_SHA': workflow_sha,
               'GITHUB_EVENT_NAME': event, 'GITHUB_REF': 'refs/heads/master'}
        run = {'status': 'completed', 'conclusion': 'success', 'head_branch': 'master',
               'event': 'push', 'path': '.github/workflows/release.yml', 'head_sha': head}
        names = ['Release gate', 'Release readiness', 'Ubuntu x86-64 DEB',
                 'Windows x86-64 portable ZIP', 'macOS x86_64', 'macOS arm64', 'Universal macOS DMG',
                 'Portable Linux (ubuntu)', 'Portable Linux (fedora)', 'Portable Linux (arch)']
        jobs = {'jobs': [{'name': name, 'conclusion': 'failure' if name == failed else 'success'} for name in names]}
        with patch.dict(os.environ, env), patch.object(release, 'api', side_effect=[run, jobs, {'commit': {'sha': master}}]):
            return release.validated_run_context('123', approved)

    def test_success(self):
        self.assertEqual(self.validate(), ('example/OpenXMB', SHA, True))

    def test_mismatched_approved_build(self):
        with self.assertRaises(RuntimeError): self.validate(head='b' * 40)

    def test_moved_master(self):
        with self.assertRaises(RuntimeError): self.validate(master='b' * 40)

    def test_wrong_workflow_checkout(self):
        with self.assertRaises(RuntimeError): self.validate(workflow_sha='b' * 40)

    def test_short_sha_is_rejected(self):
        with self.assertRaises(RuntimeError): self.validate(approved='a' * 7)

    def test_failed_package_job_is_rejected(self):
        with self.assertRaises(RuntimeError): self.validate(failed='Universal macOS DMG')

    def test_failed_cross_distribution_smoke_is_rejected(self):
        for job in ('Portable Linux (ubuntu)', 'Portable Linux (fedora)', 'Portable Linux (arch)'):
            with self.subTest(job=job), self.assertRaises(RuntimeError):self.validate(failed=job)

    def test_push_is_not_publication_authorization(self):
        with self.assertRaises(RuntimeError): self.validate(event='push')

    def test_regular_build_workflow_has_no_publication_job(self):
        workflow = (ROOT / '.github/workflows/release.yml').read_text()
        self.assertNotIn('name: Publish approved master release', workflow)


if __name__ == '__main__':
    unittest.main()
