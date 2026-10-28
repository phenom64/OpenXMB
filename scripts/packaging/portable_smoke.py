#!/usr/bin/env python3
"""Exercise a portable archive; browser integration is a separate gate."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import subprocess
import tarfile
import tempfile

import smoke


def check_integration_helpers(app, output, environment):
    for name, arguments, expected in (('openxmb-integrations', [], b'openxmb-integrations probe'),
                                      ('openxmb-retro', ['--help'], b'openxmb-retro --core')):
        result = subprocess.run([str(app / 'libexec/openxmb-exec'), str(app / 'bin' / name), *arguments], env=environment,
                                cwd=output, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30, check=True)
        if expected not in result.stdout:
            raise RuntimeError(f'{name} did not produce its expected usage output')


def make_helper_wrappers(app, directory, environment):
    """Adapt the shared smoke helper API to the packaged runtime executor."""
    result = {}
    environment = dict(environment, OPENXMB_PORTABLE_EXEC=str(app / 'libexec/openxmb-exec'),
                       OPENXMB_PORTABLE_BIN=str(app / 'bin'))
    for name in ('ffmpeg', 'ffprobe', 'pdfinfo', 'pdftoppm', 'pdftotext'):
        wrapper = directory / name
        wrapper.write_text('#!/bin/sh\nexec "$OPENXMB_PORTABLE_EXEC" "$OPENXMB_PORTABLE_BIN/${0##*/}" "$@"\n',
                           encoding='utf-8', newline='\n')
        wrapper.chmod(0o755)
        result[name] = wrapper
    return result, environment


def run_extracted(app, output, version, render):
    output.mkdir(parents=True, exist_ok=True)
    assets = app / 'share/shell'
    for name in ('Play-Regular.ttf', 'catalog/en.json', 'icons/theme/icon_setting.png',
                 'normalmaps/theme/icon_setting.png', 'sounds/ok.wav',
                 'fonts/NotoSansDevanagari-Regular.ttf', 'fonts/NotoSansCJK-Regular.ttc'):
        if not (assets / name).is_file():
            raise RuntimeError('Missing packaged asset: ' + name)
    for language in (smoke.ROOT / 'po/LINGUAS').read_text().split():
        if not (app / 'share/locale' / language / 'LC_MESSAGES/shell.mo').is_file():
            raise RuntimeError('Missing compiled translation catalog: ' + language)
    environment = os.environ.copy()
    for key in ('LD_LIBRARY_PATH', 'XMB_ASSET_DIR', 'XMB_LOCALE_DIR', 'OPENXMB_CONFIG', 'OPENXMB_DATA_DIR'):
        environment.pop(key, None)
    launcher = app / 'OpenXMB'
    for option in ('--help', '--version'):
        result = smoke.checked_output([launcher, option], environment, output,
                                      output / (option[2:] + '.log'))
        if option == '--version' and version not in result:
            raise RuntimeError('Portable executable reports the wrong release version')
    # Exercise the exact same runtime selection as XMS and the browser helper.
    with tempfile.TemporaryDirectory(prefix='openxmb-portable-helper-') as temporary:
        helpers, helper_environment = make_helper_wrappers(app, Path(temporary), environment)
        for name, path in helpers.items():
            subprocess.run([str(path), '-version' if name.startswith('ff') else '-v'],
                           env=helper_environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=30, check=True)
        smoke.check_helpers(helpers, output, helper_environment)
        check_integration_helpers(app, output, helper_environment)
    checks = ['assets and locales', 'launcher startup and version', 'integration, PDF and media helpers']
    if render:
        with tempfile.TemporaryDirectory(prefix='openxmb-portable-render-') as temporary:
            work = Path(temporary)
            config = json.loads((app / 'share/OpenXMB/config.json').read_text())
            config['render'].update({'sample-count': 1, 'vsync': False, 'max-fps': 30,
                                     'show-fps': False, 'show-mem': False})
            config['shell'].update({'font-path': 'default', 'background-type': 'original',
                                    'theme-colour-mode': 'custom', 'theme-custom-colour': '#512360'})
            (work / 'config.json').write_text(json.dumps(config))
            frames = work / 'frames'
            frames.mkdir()
            forced_directory = 'relative library ü'
            (work / forced_directory).mkdir()
            render_env = dict(environment, OPENXMB_CONFIG=str(work / 'config.json'),
                              OPENXMB_DATA_DIR=str(work / 'data'), SDL_AUDIODRIVER='dummy',
                              SDL_VIDEODRIVER='dummy', AUROREENGINE_HEADLESS='1',
                              AUROREENGINE_HEADLESS_AUDIO='1', AUROREENGINE_HEADLESS_WIDTH='960',
                              AUROREENGINE_HEADLESS_HEIGHT='640', AUROREENGINE_HEADLESS_FRAMES='16',
                              AUROREENGINE_HEADLESS_OUTPUT_DIR=str(frames),
                              AUROREENGINE_HEADLESS_OUTPUT_PATTERN='frame-{:06d}.png',
                              OPENXMB_HEADLESS_STARTUP='0', OPENXMB_FIXED_BOOT_SECONDS='17',
                              OPENXMB_FIXED_UNIX_SECONDS='1781524800', OPENXMB_FIXED_WAVE_SECONDS='4')
            smoke.checked_output([launcher, '--width', '960', '--height', '640', '--no-fullscreen',
                                  '--force-dir', forced_directory],
                                 render_env, work, output / 'render.log', timeout=180)
            images = sorted(frames.glob('*.png'))
            if len(images) < 5:
                raise RuntimeError('Portable renderer did not produce frames')
            from PIL import Image, ImageStat
            image = Image.open(images[-1]).convert('RGB')
            if image.size != (960, 640) or max(ImageStat.Stat(image).stddev) < 3:
                raise RuntimeError('Portable renderer produced a blank image')
            image.save(output / 'render.png')
        checks.append('offscreen Vulkan rendering and relative --force-dir')
    (output / 'report.json').write_text(json.dumps({'status': 'passed', 'checks': checks,
                                                    'browser': 'not tested'}, indent=2) + '\n')
    print('PASS portable smoke: ' + ', '.join(checks) + '; browser not tested')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sources = parser.add_mutually_exclusive_group(required=True)
    sources.add_argument('--archive', type=Path, help='portable tar.gz to extract and test')
    sources.add_argument('--app', type=Path, help='already extracted archive root')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--version', help='expected version; defaults to the repository VERSION')
    parser.add_argument('--render', action='store_true', help='render (always enabled)')
    args = parser.parse_args()
    output = args.output.resolve()
    version = args.version or (smoke.ROOT / 'VERSION').read_text(encoding='utf-8').strip()
    if args.app:
        run_extracted(args.app.resolve(), output, version, True)
        return
    archive = args.archive.resolve()
    match = re.fullmatch(r'OpenXMB-([A-Za-z0-9][A-Za-z0-9.+_-]*)-linux-x86_64\.tar\.gz', archive.name)
    if not match:
        raise ValueError('Unexpected portable archive filename: ' + archive.name)
    if version != match.group(1):
        raise ValueError('Expected version does not match archive filename')
    checksum = archive.with_name(archive.name + '.sha256')
    if checksum.exists():
        content = checksum.read_text(encoding='utf-8').strip()
        entry = re.fullmatch(r'([0-9a-fA-F]{64}) [ *](.+)', content)
        if not entry or entry.group(2) != archive.name:
            raise ValueError('Invalid archive checksum file')
        hasher = hashlib.sha256()
        with archive.open('rb') as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                hasher.update(chunk)
        if hasher.hexdigest().lower() != entry.group(1).lower():
            raise ValueError('Archive checksum mismatch')
    expected_root = archive.name.removesuffix('.tar.gz')
    with tempfile.TemporaryDirectory(prefix='openxmb portable test ü ') as temporary:
        root = Path(temporary)
        with tarfile.open(archive, 'r:gz') as tar:
            for member in tar.getmembers():
                path = PurePosixPath(member.name)
                if ('\\' in member.name or path.is_absolute() or '..' in path.parts
                        or not path.parts or path.parts[0] != expected_root):
                    raise ValueError('Unexpected archive member: ' + member.name)
                if not (member.isdir() or member.isfile() or member.issym() or member.islnk()):
                    raise ValueError('Unsupported special archive member: ' + member.name)
                if len(path.parts) == 1 and not member.isdir():
                    raise ValueError('Archive root must be a directory')
                if member.issym() or member.islnk():
                    link = PurePosixPath(member.linkname)
                    if '\\' in member.linkname or link.is_absolute():
                        raise ValueError('Unsafe archive link: ' + member.name)
                    base = str(path.parent) if member.issym() else ''
                    resolved = PurePosixPath(posixpath.normpath(posixpath.join(base, member.linkname)))
                    if not resolved.parts or resolved.parts[0] != expected_root or '..' in resolved.parts:
                        raise ValueError('Unsafe archive link: ' + member.name)
            tar.extractall(root, filter='data')
        app = root / expected_root
        if not app.is_dir():
            raise ValueError('Archive does not contain the expected application directory')
        run_extracted(app, output, version, True)


if __name__ == '__main__':
    main()
