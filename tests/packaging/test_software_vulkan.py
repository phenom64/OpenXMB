"""Test-driver downloads must recover from transient failures without masking errors."""
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import urllib.error
import zipfile

spec = importlib.util.spec_from_file_location('software_vulkan', Path(__file__).resolve().parents[2] / 'scripts/ci/software_vulkan.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class SoftwareVulkanDownloadTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.archive = Path(self.temp.name) / 'chrome.zip'
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w') as archive:
            archive.writestr('vk_swiftshader_icd.json', '{}')
        self.contents = data.getvalue()

    def test_dns_failure_retries_and_commits_complete_archive(self):
        with patch.object(module.urllib.request, 'urlopen', side_effect=[
                urllib.error.URLError('temporary DNS failure'), io.BytesIO(self.contents)]), \
             patch.object(module.time, 'sleep') as delay:
            module.download_archive('https://example.test/driver.zip', self.archive)
        self.assertEqual(self.archive.read_bytes(), self.contents)
        delay.assert_called_once_with(2)
        self.assertFalse(self.archive.with_suffix('.download').exists())

    def test_exhausted_retries_fail_without_a_cached_partial_file(self):
        with patch.object(module.urllib.request, 'urlopen', side_effect=urllib.error.URLError('DNS unavailable')) as request, \
             patch.object(module.time, 'sleep'):
            with self.assertRaises(urllib.error.URLError):
                module.download_archive('https://example.test/driver.zip', self.archive)
        self.assertEqual(request.call_count, 4)
        self.assertFalse(self.archive.exists())
        self.assertFalse(self.archive.with_suffix('.download').exists())

    def test_corrupt_response_is_retried(self):
        with patch.object(module.urllib.request, 'urlopen', side_effect=[io.BytesIO(b'partial ZIP'), io.BytesIO(self.contents)]), \
             patch.object(module.time, 'sleep'):
            module.download_archive('https://example.test/driver.zip', self.archive)
        self.assertEqual(self.archive.read_bytes(), self.contents)

    def test_permanent_http_error_fails_immediately(self):
        with patch.object(module.urllib.request, 'urlopen', side_effect=urllib.error.HTTPError(
                'https://example.test/driver.zip', 404, 'not found', {}, None)) as request, \
             patch.object(module.time, 'sleep'):
            with self.assertRaises(urllib.error.HTTPError):
                module.download_archive('https://example.test/driver.zip', self.archive)
        self.assertEqual(request.call_count, 1)
