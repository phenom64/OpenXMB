"""Packaging diagnostics must not turn failed native tests into a passing job."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'build_driver', Path(__file__).resolve().parents[2] / 'scripts/ci/build.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class BuildDriverTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='OpenXMB build contract ')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.stage = self.root / 'stage'
        self.outputs = self.root / 'outputs'

    def invoke(self, test_status=0, build_status=0, fail_command=None):
        def command(*args):
            if fail_command and fail_command in map(str, args):
                raise subprocess.CalledProcessError(1, list(args))

        arguments = ['build.py', '--arch', 'x86_64', '--build-dir', str(self.root / 'build'),
                     '--stage', str(self.stage)]
        with patch.object(sys, 'argv', arguments), \
             patch.dict(os.environ, {'GITHUB_OUTPUT': str(self.outputs)}, clear=True), \
             patch.object(build.platform, 'system', return_value='Linux'), \
             patch.object(build.shutil, 'which', return_value='/tools/gettext'), \
             patch.object(build, 'validate_media_tools'), \
             patch.object(build, 'run', side_effect=command), \
             patch.object(build.subprocess, 'run', side_effect=[
                 subprocess.CompletedProcess(['cmake', '--build'], build_status),
                 subprocess.CompletedProcess(['ctest'], test_status)]) as process, \
             patch.object(build.subprocess, 'check_output', return_value='tested-sha\n'):
            build.main()
            return process.call_args_list[0].args[0]

    def test_build_uses_current_runner_cpu_count(self):
        with patch.object(build.os, 'cpu_count', return_value=4):
            command = self.invoke()
        self.assertEqual(command[command.index('--parallel') + 1], '4')

    def test_successful_build_exposes_installed_stage(self):
        self.invoke()
        self.assertTrue((self.stage / 'build-info.json').is_file())
        self.assertEqual(self.outputs.read_text(), 'installed=true\n')

    def test_failed_tests_stage_diagnostics_but_still_fail_the_job(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.invoke(test_status=8)
        self.assertTrue((self.stage / 'build-info.json').is_file())
        self.assertEqual(self.outputs.read_text(), 'installed=true\n')

    def test_missing_product_after_failed_compilation_cannot_enable_packaging(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.invoke(build_status=1, fail_command='--install')
        self.assertFalse(self.outputs.exists())

    def test_failed_test_compilation_can_stage_complete_product_but_still_fails(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.invoke(build_status=1, test_status=8)
        self.assertTrue((self.stage / 'build-info.json').is_file())
        self.assertEqual(self.outputs.read_text(), 'installed=true\n')

    def test_failed_installation_cannot_enable_packaging(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.invoke(fail_command='--install')
        self.assertFalse(self.outputs.exists())


class MediaToolPreflightTests(unittest.TestCase):
    def test_windows_manifest_declares_playback_and_fixture_codecs(self):
        manifest = json.loads((build.ROOT / 'vcpkg.json').read_text())
        dependencies = {entry['name']: set(entry.get('features', []))
                        for entry in manifest['dependencies'] if isinstance(entry, dict)}
        self.assertTrue({'libflac', 'mpg123', 'opusfile', 'libmodplug'} <= dependencies['sdl2-mixer'])
        self.assertTrue({'ffmpeg', 'ffprobe', 'mp3lame', 'vorbis', 'zlib'} <= dependencies['ffmpeg'])

    def test_missing_fixture_encoder_is_reported_before_compilation(self):
        with patch.object(build.shutil, 'which', return_value='/tools/ffmpeg'), \
             patch.object(build.subprocess, 'check_output', return_value=' A..... flac\n'):
            with self.assertRaisesRegex(RuntimeError, 'libmp3lame'):
                build.validate_media_tools()

    def test_complete_fixture_capabilities_pass(self):
        encoders = 'libmp3lame libvorbis flac ffv1 mpeg4 aac pcm_s16le mjpeg png'
        filters = 'color sine testsrc2'
        listings = ['\n'.join('flags ' + name for name in values.split())
                    for values in [encoders, filters]]
        with patch.object(build.shutil, 'which', return_value='/tools/ffmpeg'), \
             patch.object(build.subprocess, 'check_output', side_effect=listings):
            build.validate_media_tools()

    def test_all_capability_groups_are_reported_together(self):
        with patch.object(build.shutil, 'which', return_value='/tools/ffmpeg'), \
             patch.object(build.subprocess, 'check_output', side_effect=[' A..... flac\n', ' ... sine\n']):
            with self.assertRaises(RuntimeError) as failure:
                build.validate_media_tools()
        self.assertIn('-encoders:', str(failure.exception))
        self.assertIn('-filters:', str(failure.exception))


if __name__ == '__main__':
    unittest.main()
