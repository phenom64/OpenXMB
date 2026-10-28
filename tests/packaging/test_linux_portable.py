import importlib.util
import hashlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[2] / 'scripts/packaging/linux_portable.py'
SMOKE_SCRIPT = SCRIPT.with_name('portable_smoke.py')


class PortableTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = None
        if SCRIPT.exists():
            spec = importlib.util.spec_from_file_location('linux_portable', SCRIPT)
            cls.module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(cls.module)

    def test_dependency_parser_and_missing(self):
        self.assertIsNotNone(self.module, 'portable packager is missing')
        parsed = self.module.parse_ldd('libfoo.so.1 => /tmp/libfoo.so.1 (0x123)\nlibc.so.6 => /lib/libc.so.6 (0x456)\n')
        self.assertEqual(parsed['libfoo.so.1'], '/tmp/libfoo.so.1')
        with self.assertRaisesRegex(RuntimeError, 'liblost.so'):
            self.module.parse_ldd('liblost.so => not found\n')

    def test_host_libraries_and_driver_exclusions(self):
        self.assertIsNotNone(self.module, 'portable packager is missing')
        for name in ('libc.so.6', 'ld-linux-x86-64.so.2', 'libpthread.so.0', 'libnsl.so.1',
                     'libGLX_nvidia.so.0', 'libvulkan_radeon.so', 'radeonsi_dri.so'):
            self.assertTrue(self.module.host_only(name), name)
        for name in ('libvulkan.so.1', 'libavcodec.so.60', 'libwebkit2gtk-4.1.so.0'):
            self.assertFalse(self.module.host_only(name), name)

    def test_launcher_preserves_cwd_and_arguments(self):
        self.assertIsNotNone(self.module, 'portable packager is missing')
        launcher = self.module.launcher_script()
        self.assertIn('exec "$DIR/bin/XMS.bin" "$@"', launcher)
        self.assertNotIn('cd "$DIR"\n', launcher)
        self.assertIn('XMB_ASSET_DIR', launcher)
        self.assertIn('XMB_LOCALE_DIR', launcher)

    def test_webkit_relocation_requires_unambiguous_path(self):
        self.assertIsNotNone(self.module, 'portable packager is missing')
        old = b'/usr/lib/x86_64-linux-gnu/webkit2gtk-4.1'
        result = self.module.relocate_webkit_bytes(b'prefix\0' + old + b'\0suffix', old)
        self.assertEqual(len(result), len(b'prefix\0' + old + b'\0suffix'))
        self.assertIn(b'./libexec/webkit\0', result)
        with self.assertRaisesRegex(RuntimeError, 'compiled-in'):
            self.module.relocate_webkit_bytes(b'no path', old)

    def test_resource_contract_rejects_missing_browser(self):
        self.assertIsNotNone(self.module, 'portable packager is missing')
        with self.assertRaisesRegex(RuntimeError, 'openxmb-web'):
            self.module.validate_stage(Path(__file__).parent)

    def test_portable_notes_are_copied_into_archive_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'source'
            (root / 'docs').mkdir(parents=True)
            notes = b'# Portable notes\nLaunch and AppArmor.\n'
            (root / 'docs/linux-portable.md').write_bytes(notes)
            app = Path(tmp) / 'app'
            app.mkdir()
            with patch.object(self.module, 'ROOT', root):
                self.module.copy_portable_notes(app)
            self.assertEqual((app / 'PORTABLE.md').read_bytes(), notes)

    def test_elf_collection_copies_transitive_dependencies_and_detects_conflict(self):
        self.assertIsNotNone(self.module, 'portable packager is missing')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            app = root / 'app'
            (app / 'bin').mkdir(parents=True)
            (app / 'bin/XMS.bin').write_bytes(b'\x7fELF app')
            one, two = root / 'one', root / 'two'
            one.mkdir()
            two.mkdir()
            (one / 'libone.so').write_bytes(b'\x7fELF one')
            (two / 'libtwo.so').write_bytes(b'\x7fELF two')
            def ldd(_program, binary):
                if binary.name == 'XMS.bin':
                    return f'libone.so => /fixture/one/libone.so (0x123)\n'
                if binary.name == 'libone.so':
                    return f'libtwo.so => /fixture/two/libtwo.so (0x123)\n'
                return ''
            # The parser normally receives Unix paths; fixture paths on Windows
            # need a small adapter while exercising the real collection logic.
            def fixture_parse(text):
                result = {}
                for line in text.splitlines():
                    name = line.split(' => ')[0]
                    result[name] = str((one if name == 'libone.so' else two) / name)
                return result
            with patch.object(self.module, 'command', side_effect=ldd), patch.object(self.module, 'parse_ldd', side_effect=fixture_parse):
                self.module.collect_elf_dependencies(app)
            self.assertEqual((app / 'lib/libtwo.so').read_bytes(), b'\x7fELF two')
            (two / 'libtwo.so').write_bytes(b'\x7fELF changed')
            with patch.object(self.module, 'command', side_effect=ldd), patch.object(self.module, 'parse_ldd', side_effect=fixture_parse):
                with self.assertRaisesRegex(RuntimeError, 'Conflicting ELF dependency'):
                    self.module.collect_elf_dependencies(app)

    def test_launcher_executes_from_unusual_path_without_changing_cwd(self):
        self.assertIsNotNone(self.module, 'portable packager is missing')
        bash = Path('C:/Program Files/Git/bin/bash.exe') if os.name == 'nt' else Path('/bin/sh')
        if not bash.exists():
            self.skipTest('no POSIX shell available')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'space ü'
            (root / 'bin').mkdir(parents=True)
            launcher = root / 'OpenXMB'
            launcher.write_text(self.module.launcher_script(), encoding='utf-8')
            target = root / 'bin/XMS.bin'
            target.write_text('#!/bin/sh\nprintf "%s\\n" "$PWD" "$@" > "$CAPTURE"\n', encoding='utf-8')
            target.chmod(0o755)
            capture = Path(tmp) / 'args'
            # Git Bash translates Windows environment paths to Unix paths.
            env = dict(os.environ, CAPTURE=str(capture))
            subprocess.run([str(bash), str(launcher), '--force-dir', 'folder with space', 'ユニコード'],
                           cwd=tmp, env=env, check=True)
            lines = capture.read_text(encoding='utf-8').splitlines()
            self.assertEqual(lines[1:], ['--force-dir', 'folder with space', 'ユニコード'])
            self.assertIn(Path(tmp).name, lines[0])

    def test_fonts_are_required_and_bundled_at_lookup_paths(self):
        self.assertIsNotNone(self.module, 'portable packager is missing')
        self.assertEqual(self.module.FONTS[0][1], 'NotoSansDevanagari-Regular.ttf')
        self.assertEqual(self.module.FONTS[1][1], 'NotoSansCJK-Regular.ttc')

    def test_runtime_fallback_never_overrides_compatible_host(self):
        bash = Path('C:/Program Files/Git/bin/bash.exe') if os.name == 'nt' else Path('/bin/sh')
        if not bash.exists():
            self.skipTest('no POSIX shell available')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'portable'
            fake = root / 'fake'
            fake.mkdir(parents=True)
            (root / 'bin').mkdir()
            for item, lib, version in (('cxx', 'libstdc++.so.6', 'GLIBCXX_3.4.30\nCXXABI_1.3.13'),
                                       ('gcc', 'libgcc_s.so.1', 'GCC_3.0\nGCC_4.2.0')):
                fallback = root / 'lib/fallback' / item
                fallback.mkdir(parents=True)
                (fallback / lib).write_text('fake')
                (fallback / 'required-versions').write_text(version + '\n', newline='\n')
                (fake / lib).write_text(version + '\n', newline='\n')
            ldconfig = fake / 'ldconfig'
            ldconfig.write_text('#!/bin/sh\np=$(dirname "$0")\nprintf "libstdc++.so.6 (libc6,x86-64) => %s/libstdc++.so.6\\nlibgcc_s.so.1 (libc6,x86-64) => %s/libgcc_s.so.1\\n" "$p" "$p"\n')
            ldconfig.chmod(0o755)
            readelf = root / 'bin/openxmb-readelf'
            readelf.write_text('#!/bin/sh\nprintf "Version definition section .gnu.version_d:\\n"\nwhile IFS= read -r n; do printf " Name: %s\\n" "$n"; done < "$2"\nprintf "Version needs section .gnu.version_r:\\n Name: CXXABI_1.3.13\\n"\n')
            readelf.chmod(0o755)
            (root / 'OpenXMB').write_text(self.module.launcher_script(), encoding='utf-8')
            target = root / 'bin/XMS.bin'
            target.write_text('#!/bin/sh\nprintf "%s" "$LD_LIBRARY_PATH" > "$CAPTURE"\n')
            target.chmod(0o755)
            bootstrap = root / 'start'
            bootstrap.write_text('#!/bin/sh\nDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)\nPATH="$DIR/fake:$PATH"\nexport PATH\nexec "$DIR/OpenXMB"\n', encoding='utf-8')
            (root / 'OpenXMB').chmod(0o755)
            capture = Path(tmp) / 'runtime'
            env = dict(os.environ, CAPTURE=str(capture))
            subprocess.run([str(bash), str(bootstrap)], env=env, check=True)
            self.assertNotIn('/fallback/cxx', capture.read_text())
            self.assertNotIn('/fallback/gcc', capture.read_text())
            # A high GLIBCXX node is insufficient when a different required
            # CXXABI node is only present as an import, not a definition.
            (fake / 'libstdc++.so.6').write_text('GLIBCXX_3.4.30\nCXXABI_1.3.12\n', newline='\n')
            subprocess.run([str(bash), str(bootstrap)], env=env, check=True)
            self.assertIn('/fallback/cxx', capture.read_text())
            self.assertNotIn('/fallback/gcc', capture.read_text())
            (fake / 'libstdc++.so.6').write_text('GLIBCXX_3.4.30\nCXXABI_1.3.13\n', newline='\n')
            (fake / 'libgcc_s.so.1').write_text('GCC_4.2.0\n', newline='\n')
            subprocess.run([str(bash), str(bootstrap)], env=env, check=True)
            self.assertNotIn('/fallback/cxx', capture.read_text())
            self.assertIn('/fallback/gcc', capture.read_text())
            (root / 'libexec').mkdir()
            executor = root / 'libexec/openxmb-exec'
            executor.write_text(self.module.portable_executor_script(), encoding='utf-8')
            executor.chmod(0o755)
            bootstrap.write_text('#!/bin/sh\nDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)\nPATH="$DIR/fake:$PATH"\nexport PATH\nexec "$DIR/libexec/openxmb-exec" "$DIR/bin/XMS.bin"\n')
            subprocess.run([str(bash), str(bootstrap)], env=env, check=True)
            self.assertNotIn('/fallback/cxx', capture.read_text())
            self.assertIn('/fallback/gcc', capture.read_text())

    def test_fallback_manifests_include_all_version_needs(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = Path(tmp)
            (app / 'bin').mkdir()
            (app / 'lib').mkdir()
            for path in ('bin/XMS.bin', 'lib/libstdc++.so.6', 'lib/libgcc_s.so.1'):
                (app / path).write_bytes(b'\x7fELF fixture')
            info = '''Version needs section '.gnu.version_r' contains 2 entries:
  000000: Version: 1  File: libstdc++.so.6  Cnt: 3
  0x0010: Name: GLIBCXX_3.4.30  Flags: none  Version: 4
  0x0020: Name: GLIBCXX_3.4.21  Flags: none  Version: 5
  0x0030: Name: CXXABI_1.3.13  Flags: none  Version: 6
  0x0040: Version: 1  File: libgcc_s.so.1  Cnt: 2
  0x0050: Name: GCC_3.0  Flags: none  Version: 7
  0x0060: Name: GCC_4.2.0  Flags: none  Version: 8
'''
            with patch.object(self.module, 'command', return_value=info):
                self.module.prepare_fallbacks(app)
            self.assertEqual(set((app / 'lib/fallback/cxx/required-versions').read_text().split()),
                             {'GLIBCXX_3.4.30', 'GLIBCXX_3.4.21', 'CXXABI_1.3.13'})
            self.assertEqual(set((app / 'lib/fallback/gcc/required-versions').read_text().split()),
                             {'GCC_3.0', 'GCC_4.2.0'})

    def test_libstdcpp_gcc_needs_are_included_without_its_definitions(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = Path(tmp)
            (app / 'bin').mkdir()
            (app / 'lib').mkdir()
            for path in ('bin/XMS.bin', 'lib/libstdc++.so.6', 'lib/libgcc_s.so.1'):
                (app / path).write_bytes(b'\x7fELF fixture')
            def version_info(_tool, _option, path):
                if path.name.startswith('libstdc++'):
                    return ("Version definition section '.gnu.version_d':\n"
                            '  Name: GLIBCXX_9.9\n  Name: CXXABI_9.9\n'
                            "Version needs section '.gnu.version_r':\n"
                            '  File: libgcc_s.so.1  Cnt: 1\n  Name: GCC_7.7\n')
                if path.name.startswith('libgcc'):
                    return "Version definition section '.gnu.version_d':\n  Name: GCC_9.9\n"
                return ("Version needs section '.gnu.version_r':\n"
                        '  File: libstdc++.so.6  Cnt: 1\n  Name: GLIBCXX_3.4.30\n')
            with patch.object(self.module, 'command', side_effect=version_info):
                self.module.prepare_fallbacks(app)
            self.assertEqual((app / 'lib/fallback/cxx/required-versions').read_text().split(), ['GLIBCXX_3.4.30'])
            self.assertEqual((app / 'lib/fallback/gcc/required-versions').read_text().split(), ['GCC_7.7'])

    def test_gio_querymodules_resolution_precedence_and_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            fallback = Path(tmp) / 'gio-querymodules'
            fallback.write_text('fixture')
            fallback.chmod(0o755)
            with patch.object(self.module.shutil, 'which', return_value='/path/gio-querymodules'):
                self.assertEqual(self.module.resolve_gio_querymodules(fallback), '/path/gio-querymodules')
            with patch.object(self.module.shutil, 'which', return_value=None):
                self.assertEqual(self.module.resolve_gio_querymodules(fallback), str(fallback))
                with self.assertRaisesRegex(RuntimeError, 'gio-querymodules'):
                    self.module.resolve_gio_querymodules(Path(tmp) / 'missing')

    def test_profile_targets_binary_and_rejects_patterns(self):
        bash = Path('C:/Program Files/Git/bin/bash.exe') if os.name == 'nt' else Path('/bin/sh')
        if not bash.exists():
            self.skipTest('no POSIX shell available')
        with tempfile.TemporaryDirectory() as tmp:
            for folder, ok in (('safe ü', True), ('bad[pattern]', False)):
                root = Path(tmp) / folder
                root.mkdir()
                script = root / 'print-apparmor-profile'
                script.write_text(self.module.apparmor_profile_script(), encoding='utf-8')
                result = subprocess.run([str(bash), str(script)], capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, ok, result.stderr)
                if ok:
                    self.assertIn('/bin/openxmb-web.bin" flags=(unconfined)', result.stdout)
                    self.assertIn('  userns,', result.stdout)

    def test_archive_cli_extracts_and_enables_render(self):
        with patch.dict(sys.modules), patch.object(sys, 'path', [str(SCRIPT.parent), *sys.path]):
            spec = importlib.util.spec_from_file_location('portable_smoke', SMOKE_SCRIPT)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            with tempfile.TemporaryDirectory() as tmp:
                archive = Path(tmp) / 'OpenXMB-0.3.0-beta.3-linux-x86_64.tar.gz'
                payload = b'fixture'
                with tarfile.open(archive, 'w:gz') as tar:
                    entry = tarfile.TarInfo('OpenXMB-0.3.0-beta.3-linux-x86_64/fixture')
                    entry.size = len(payload)
                    tar.addfile(entry, io.BytesIO(payload))
                def check_extracted(app, output, version, render):
                    self.assertEqual((app / 'fixture').read_bytes(), payload)
                    self.assertIn('portable test ü', str(app))
                    self.assertEqual(version, '0.3.0-beta.3')
                    self.assertTrue(render)
                    self.assertEqual(output, (Path(tmp) / 'output').resolve())
                with patch.object(module, 'run_extracted', side_effect=check_extracted) as run:
                    with patch.object(sys, 'argv', ['portable_smoke.py', '--archive', str(archive), '--output', str(Path(tmp) / 'output')]):
                        module.main()
                run.assert_called_once()

    def test_archive_rejects_escape(self):
        with patch.dict(sys.modules), patch.object(sys, 'path', [str(SCRIPT.parent), *sys.path]):
            spec = importlib.util.spec_from_file_location('portable_smoke', SMOKE_SCRIPT)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            with tempfile.TemporaryDirectory() as tmp:
                archive = Path(tmp) / 'OpenXMB-0.3.0-beta.3-linux-x86_64.tar.gz'
                with tarfile.open(archive, 'w:gz') as tar:
                    entry = tarfile.TarInfo('../../escape')
                    tar.addfile(entry, io.BytesIO())
                with patch.object(sys, 'argv', ['portable_smoke.py', '--archive', str(archive), '--output', str(Path(tmp) / 'output')]):
                    with self.assertRaises((ValueError, RuntimeError, tarfile.TarError)):
                        module.main()

    def test_archive_rejects_special_files_and_escaping_links(self):
        with patch.dict(sys.modules), patch.object(sys, 'path', [str(SCRIPT.parent), *sys.path]):
            spec = importlib.util.spec_from_file_location('portable_smoke', SMOKE_SCRIPT)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            with tempfile.TemporaryDirectory() as tmp:
                archive = Path(tmp) / 'OpenXMB-0.3.0-beta.3-linux-x86_64.tar.gz'
                root = archive.name.removesuffix('.tar.gz')
                for kind, target in ((tarfile.FIFOTYPE, ''), (tarfile.SYMTYPE, '../../escape'),
                                     (tarfile.LNKTYPE, '/tmp/escape')):
                    with tarfile.open(archive, 'w:gz') as tar:
                        entry = tarfile.TarInfo(root + '/invalid')
                        entry.type = kind
                        entry.linkname = target
                        tar.addfile(entry)
                    with patch.object(sys, 'argv', ['portable_smoke.py', '--archive', str(archive), '--output', str(Path(tmp) / 'output')]):
                        with self.assertRaises((ValueError, RuntimeError, tarfile.TarError)):
                            module.main()

    def test_archive_checksum_and_repo_version_are_enforced(self):
        with patch.dict(sys.modules), patch.object(sys, 'path', [str(SCRIPT.parent), *sys.path]):
            spec = importlib.util.spec_from_file_location('portable_smoke', SMOKE_SCRIPT)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            with tempfile.TemporaryDirectory() as tmp:
                archive = Path(tmp) / 'OpenXMB-0.3.0-beta.3-linux-x86_64.tar.gz'
                with tarfile.open(archive, 'w:gz') as tar:
                    entry = tarfile.TarInfo(archive.name.removesuffix('.tar.gz'))
                    entry.type = tarfile.DIRTYPE
                    tar.addfile(entry)
                checksum = Path(str(archive) + '.sha256')
                checksum.write_text('0' * 64 + '  ' + archive.name + '\n')
                with patch.object(sys, 'argv', ['portable_smoke.py', '--archive', str(archive), '--output', str(Path(tmp) / 'output')]):
                    with self.assertRaisesRegex(ValueError, 'checksum'):
                        module.main()
                    checksum.write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + archive.name + '\n')
                    with patch.object(module, 'run_extracted') as run:
                        module.main()
                        run.assert_called_once()
                    with patch.object(module.smoke, 'ROOT', Path(tmp)):
                        (Path(tmp) / 'VERSION').write_text('9.9.9')
                        with self.assertRaisesRegex(ValueError, 'version'):
                            module.main()

    def test_integration_helpers_check_documented_output(self):
        with patch.dict(sys.modules), patch.object(sys, 'path', [str(SCRIPT.parent), *sys.path]):
            spec = importlib.util.spec_from_file_location('portable_smoke', SMOKE_SCRIPT)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            fake_results = [subprocess.CompletedProcess([], 0, b'openxmb-integrations probe [PATH]'),
                            subprocess.CompletedProcess([], 0, b'openxmb-retro --core LIBRARY')]
            with patch.object(module.subprocess, 'run', side_effect=fake_results) as run:
                module.check_integration_helpers(Path('/app'), Path('/output'), {})
            self.assertEqual(run.call_args_list[0].args[0], [str(Path('/app') / 'libexec/openxmb-exec'), str(Path('/app') / 'bin/openxmb-integrations')])
            self.assertEqual(run.call_args_list[1].args[0][-1], '--help')
            with patch.object(module.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, b'unexpected')):
                with self.assertRaisesRegex(RuntimeError, 'usage'):
                    module.check_integration_helpers(Path('/app'), Path('/output'), {})

    def test_smoke_media_helpers_use_packaged_executor(self):
        with patch.dict(sys.modules), patch.object(sys, 'path', [str(SCRIPT.parent), *sys.path]):
            spec = importlib.util.spec_from_file_location('portable_smoke', SMOKE_SCRIPT)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            with tempfile.TemporaryDirectory() as tmp:
                helpers, env = module.make_helper_wrappers(Path('/app'), Path(tmp), {})
                self.assertEqual(env['OPENXMB_PORTABLE_EXEC'], str(Path('/app/libexec/openxmb-exec')))
                self.assertEqual(env['OPENXMB_PORTABLE_BIN'], str(Path('/app/bin')))
                for name, wrapper in helpers.items():
                    self.assertEqual(wrapper.name, name)
                    self.assertIn('"$OPENXMB_PORTABLE_EXEC"', wrapper.read_text())
                bash = Path('C:/Program Files/Git/bin/bash.exe') if os.name == 'nt' else Path('/bin/sh')
                if not bash.exists():
                    self.skipTest('no POSIX shell available')
                executor = Path(tmp) / 'fake-executor'
                executor.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$CAPTURE"\n', newline='\n')
                executor.chmod(0o755)
                bootstrap = Path(tmp) / 'start'
                bootstrap.write_text('#!/bin/sh\nDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)\n'
                                     'export OPENXMB_PORTABLE_EXEC="$DIR/fake-executor" OPENXMB_PORTABLE_BIN="$DIR/bin"\n'
                                     'exec "$DIR/ffprobe" -v error\n', newline='\n')
                capture = Path(tmp) / 'captured'
                subprocess.run([str(bash), str(bootstrap)], env=dict(os.environ, CAPTURE=str(capture)), check=True)
                lines = capture.read_text().splitlines()
                self.assertTrue(lines[0].endswith('/bin/ffprobe'))
                self.assertEqual(lines[1:], ['-v', 'error'])

    def test_web_wrapper_sets_runtime_environment_when_invoked_directly(self):
        bash = Path('C:/Program Files/Git/bin/bash.exe') if os.name == 'nt' else Path('/bin/sh')
        if not bash.exists():
            self.skipTest('no POSIX shell available')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'archive ü'
            (root / 'bin').mkdir(parents=True)
            wrapper = root / 'bin/openxmb-web'
            wrapper.write_text(self.module.browser_launcher_script(), encoding='utf-8')
            binary = root / 'bin/openxmb-web.bin'
            binary.write_text('#!/bin/sh\nprintf "%s\\n" "$LD_LIBRARY_PATH" "$GIO_MODULE_DIR" "$PWD" "$@" > "$CAPTURE"\n')
            binary.chmod(0o755)
            capture = Path(tmp) / 'result'
            env = dict(os.environ, CAPTURE=str(capture))
            env.pop('LD_LIBRARY_PATH', None)
            subprocess.run([str(bash), str(wrapper), 'https://example.test', '42', 'relative profile', 'relative state'],
                           env=env, cwd=tmp, check=True)
            lines = capture.read_text(encoding='utf-8').splitlines()
            self.assertIn('/archive ü/lib:', lines[0])
            self.assertIn('/archive ü/lib/gio/modules', lines[1])
            self.assertTrue(lines[2].endswith('/archive ü'))
            self.assertEqual(lines[3:5], ['https://example.test', '42'])
            self.assertTrue(lines[5].endswith('/relative profile'))
            self.assertTrue(lines[6].endswith('/relative state'))


if __name__ == '__main__':
    unittest.main()
