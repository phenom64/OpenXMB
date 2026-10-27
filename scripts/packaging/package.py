#!/usr/bin/env python3
"""Create relocatable desktop packages from a tested CMake installation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
VERSION = (ROOT / 'VERSION').read_text().strip()
MACHO = {b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'}


def run(*args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, **kwargs)


def output(*args):
    return subprocess.check_output([str(x) for x in args], text=True).strip()


def is_macho(path):
    return path.is_file() and not path.is_symlink() and path.open('rb').read(4) in MACHO


def notices(destination):
    destination.mkdir(parents=True, exist_ok=True)
    for name in ['LICENSE', 'LICENSE.RetroArch']:
        shutil.copy2(ROOT / name, destination / name)
    shutil.copytree(ROOT / 'LICENSES', destination / 'LICENSES', dirs_exist_ok=True)


def linux(stage, destination):
    package = destination / 'deb-root'
    shutil.copytree(stage, package / 'usr', dirs_exist_ok=True)
    control = package / 'DEBIAN'
    control.mkdir(parents=True)
    metadata = destination / 'shlibdeps'
    (metadata / 'debian').mkdir(parents=True, exist_ok=True)
    (metadata / 'debian/control').write_text(
        'Source: openxmb\nSection: graphics\nPriority: optional\n'
        'Maintainer: phenom64 <75783341+phenom64@users.noreply.github.com>\n'
        '\nPackage: openxmb\nArchitecture: amd64\nDescription: OpenXMB desktop shell\n')
    executables = [p for p in (package / 'usr/bin').iterdir()
                   if p.is_file() and p.open('rb').read(4) == b'\x7fELF']
    command = ['dpkg-shlibdeps', '-O', *['-e' + str(p) for p in executables]]
    result = subprocess.check_output(command, cwd=metadata, text=True).strip()
    dependencies = result.removeprefix('shlibs:Depends=').split(', ')
    dependencies += ['libvulkan1', 'ffmpeg', 'poppler-utils', 'xdg-utils', 'ca-certificates']
    (control / 'control').write_text(
        f'Package: openxmb\nVersion: {VERSION.replace("-beta.", "~beta.")}\n'
        'Section: graphics\nPriority: optional\nArchitecture: amd64\n'
        'Maintainer: phenom64 <75783341+phenom64@users.noreply.github.com>\n'
        f'Depends: {", ".join(sorted(set(dependencies)))}\n'
        'Recommends: mesa-vulkan-drivers | vulkan-icd\n'
        'Homepage: https://github.com/phenom64/OpenXMB\n'
        'Description: OpenXMB desktop shell\n'
        ' A controller-friendly desktop crossbar with themes and native media viewers.\n')
    applications = package / 'usr/share/applications'
    applications.mkdir(parents=True, exist_ok=True)
    (applications / 'openxmb.desktop').write_text(
        '[Desktop Entry]\nType=Application\nName=OpenXMB\nExec=XMS\n'
        'Icon=openxmb\nTerminal=false\nCategories=Game;Utility;\n')
    icon = package / 'usr/share/icons/hicolor/256x256/apps/openxmb.png'
    icon.parent.mkdir(parents=True, exist_ok=True)
    from PIL import Image
    Image.open(ROOT / 'interfaceFX/GraphicsServer/SyndromaticOrb.png').convert('RGBA').resize((256, 256)).save(icon)
    notices(package / 'usr/share/doc/openxmb')
    artifact = destination / f'openxmb_{VERSION.replace("-beta.", "~beta.")}_amd64.deb'
    run('dpkg-deb', '--root-owner-group', '--build', package, artifact)
    return artifact


def windows(stage, destination):
    app = destination / f'OpenXMB-{VERSION}-windows-x86_64'
    shutil.copytree(stage, app, dirs_exist_ok=True)
    (app / 'XMS.bin.exe').rename(app / 'OpenXMB.exe')
    prefix = Path(os.environ['VCPKG_INSTALLED_DIR']) / 'x64-windows'
    for path in (prefix / 'bin').glob('*.dll'):
        shutil.copy2(path, app / path.name)
    for name in ['ffmpeg.exe', 'ffprobe.exe']:
        source = next((prefix / 'tools/ffmpeg').rglob(name))
        shutil.copy2(source, app / name)
    poppler = Path(os.environ['OPENXMB_POPPLER_BIN'])
    shutil.copytree(poppler.parent, app / 'tools/poppler', dirs_exist_ok=True)
    redist = os.environ.get('VCToolsRedistDir')
    if not redist:
        raise RuntimeError('The MSVC redistributable directory is required')
    for path in (Path(redist) / 'x64').glob('Microsoft.VC*.CRT/*.dll'):
        shutil.copy2(path, app / path.name)
    notices(app / 'licenses')
    for path in (prefix / 'share').glob('*/copyright'):
        shutil.copy2(path, app / 'licenses' / (path.parent.name + '.txt'))
    for path in poppler.parent.rglob('*'):
        if path.is_file() and path.name.lower() in ['license', 'copying', 'copyright']:
            shutil.copy2(path, app / 'licenses' / ('poppler-' + path.parent.name + '-' + path.name))
    artifact = destination / (app.name + '.zip')
    with zipfile.ZipFile(artifact, 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(app.rglob('*')):
            if path.is_file():
                archive.write(path, Path(app.name) / path.relative_to(app))
    return artifact


def dependencies(path):
    result = []
    for arch in output('lipo', '-archs', path).split():
        identity = output('otool', '-arch', arch, '-D', path).splitlines()[1:]
        identity = identity[0].strip() if identity else None
        for line in output('otool', '-arch', arch, '-L', path).splitlines()[1:]:
            name = line.strip().split(' (', 1)[0]
            if name != identity and name not in result:
                result.append(name)
    return result


def rpaths(path):
    lines = output('otool', '-l', path).splitlines()
    return list(dict.fromkeys(lines[i + 2].strip().split('path ', 1)[1].split(' (offset', 1)[0]
                             for i, line in enumerate(lines) if line.strip() == 'cmd LC_RPATH'))


def mac_thin(stage, destination, arch):
    app = destination / 'OpenXMB.app'
    contents = app / 'Contents'
    binaries, resources, frameworks = [contents / name for name in ['MacOS', 'Resources', 'Frameworks']]
    for path in [binaries, resources, frameworks]:
        path.mkdir(parents=True, exist_ok=True)
    shutil.copytree(stage / 'share', resources / 'share', dirs_exist_ok=True)
    for path in (stage / 'bin').iterdir():
        if is_macho(path):
            shutil.copy2(path, binaries / ('OpenXMB' if path.name == 'XMS.bin' else path.name))
    for name in ['ffmpeg', 'ffprobe', 'pdftoppm', 'pdftotext', 'pdfinfo']:
        shutil.copy2(Path(shutil.which(name)).resolve(), binaries / name)
    with (contents / 'Info.plist').open('wb') as handle:
        plistlib.dump({
            'CFBundleExecutable': 'OpenXMB', 'CFBundleName': 'OpenXMB',
            'CFBundleDisplayName': 'OpenXMB', 'CFBundleIdentifier': 'com.syndromatic.openxmb',
            'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': VERSION.split('-')[0],
            'CFBundleVersion': '20002', 'OpenXMBReleaseVersion': VERSION,
            'CFBundleIconFile': 'OpenXMB.icns', 'LSMinimumSystemVersion': '15.0',
            'LSApplicationCategoryType': 'public.app-category.utilities',
            'NSHighResolutionCapable': True, 'NSPrincipalClass': 'NSApplication',
        }, handle)
    from PIL import Image
    iconset = destination / 'OpenXMB.iconset'
    iconset.mkdir(exist_ok=True)
    image = Image.open(ROOT / 'interfaceFX/GraphicsServer/SyndromaticOrb.png').convert('RGBA')
    for size in [16, 32, 128, 256, 512]:
        for scale in [1, 2]:
            image.resize((size * scale, size * scale), Image.Resampling.LANCZOS).save(
                iconset / f'icon_{size}x{size}{"@2x" if scale == 2 else ""}.png')
    run('iconutil', '-c', 'icns', iconset, '-o', resources / 'OpenXMB.icns')
    notices(resources / 'Licenses')
    brew = Path(output('brew', '--prefix'))
    certificates = resources / 'certs'
    certificates.mkdir()
    shutil.copy2(brew / 'etc/ca-certificates/cert.pem', certificates / 'cert.pem')
    poppler_data = brew / 'share/poppler'
    if poppler_data.exists():
        shutil.copytree(poppler_data, resources / 'share/poppler', dirs_exist_ok=True)
    fonts = resources / 'fonts'
    fonts.mkdir()
    (fonts / 'fonts.conf').write_text(
        '<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "fonts.dtd"><fontconfig>'
        '<dir>/System/Library/Fonts</dir><dir>/Library/Fonts</dir><dir>~/Library/Fonts</dir>'
        '<cachedir prefix="xdg">fontconfig</cachedir></fontconfig>')
    origins = {}
    queue = []
    for path in binaries.iterdir():
        if path.name == 'OpenXMB':
            source = stage / 'bin/XMS.bin'
        elif path.name.startswith('openxmb-'):
            source = stage / 'bin' / path.name
        else:
            source = Path(shutil.which(path.name)).resolve()
        queue.append((source, path))

    def add_library(source):
        source = Path(source)
        name = source.name
        if name in origins:
            if source.resolve() != origins[name].resolve() and source.read_bytes() != origins[name].read_bytes():
                raise RuntimeError('Conflicting library names: ' + name)
            return name
        if not source.is_file():
            raise RuntimeError('Missing bundled library: ' + str(source))
        origins[name] = source
        target = frameworks / name
        shutil.copy2(source, target)
        target.chmod(target.stat().st_mode | 0o200)
        run('install_name_tool', '-id', '@rpath/' + name, target)
        queue.append((source.resolve(), target))
        return name

    add_library(Path(output('brew', '--prefix', 'vulkan-loader')) / 'lib/libvulkan.1.dylib')
    molten = Path(output('brew', '--prefix', 'molten-vk'))
    molten_library = next(molten.rglob('libMoltenVK.dylib'))
    add_library(molten_library)
    while queue:
        source, target = queue.pop(0)
        for dependency in dependencies(source):
            if dependency.startswith(('/usr/lib/', '/System/Library/')):
                continue
            candidate = Path(dependency)
            if dependency.startswith('@loader_path/'):
                candidate = source.parent / dependency.removeprefix('@loader_path/')
            elif dependency.startswith(('@rpath/', '@executable_path/')):
                name = Path(dependency).name
                candidates = [brew / 'lib' / name, source.parent / name, source.parent.parent / 'lib' / name]
                candidates += list((brew / 'opt').glob('*/lib/' + name))
                candidate = next((p for p in candidates if p.is_file()), Path('/missing') / name)
            name = add_library(candidate)
            run('install_name_tool', '-change', dependency, '@executable_path/../Frameworks/' + name, target)
        paths = rpaths(target)
        for path in paths:
            if path.startswith(('/opt/homebrew/', '/usr/local/')):
                run('install_name_tool', '-delete_rpath', path, target)
        if '@executable_path/../Frameworks' not in paths:
            run('install_name_tool', '-add_rpath', '@executable_path/../Frameworks', target)
    icd = resources / 'vulkan/icd.d'
    icd.mkdir(parents=True)
    source_icd = next(molten.rglob('MoltenVK_icd.json'))
    manifest = json.loads(source_icd.read_text())
    manifest['ICD']['library_path'] = '../../../Frameworks/libMoltenVK.dylib'
    (icd / 'MoltenVK_icd.json').write_text(json.dumps(manifest, indent=2) + '\n')
    sign_and_verify(app)
    artifact = destination / f'OpenXMB-{VERSION}-macos-{arch}.tar.gz'
    with tarfile.open(artifact, 'w:gz') as archive:
        archive.add(app, arcname=app.name)
    return artifact


def sign_and_verify(app):
    for path in app.rglob('*'):
        if is_macho(path):
            run('codesign', '--force', '--sign', '-', path)
            for dependency in dependencies(path):
                if not dependency.startswith(('/usr/lib/', '/System/Library/', '@executable_path/../Frameworks/')):
                    raise RuntimeError('Unbundled runtime dependency: ' + dependency)
    run('codesign', '--force', '--deep', '--sign', '-', app)
    run('codesign', '--verify', '--deep', '--strict', app)


def universal(intel, arm, background, destination):
    roots = []
    for archive_path, name in [(intel, 'intel'), (arm, 'arm')]:
        root = destination / name
        root.mkdir()
        with tarfile.open(archive_path) as archive:
            archive.extractall(root, filter='data')
        roots.append(root / 'OpenXMB.app')
    app = destination / 'OpenXMB.app'
    shutil.copytree(roots[0], app, ignore=shutil.ignore_patterns('_CodeSignature'))
    for path in app.rglob('*'):
        if not path.is_file():
            continue
        relative = path.relative_to(app)
        counterpart = roots[1] / relative
        if not counterpart.is_file():
            raise RuntimeError('Architecture bundles differ: ' + str(relative))
        if is_macho(path):
            thin_files = []
            for root, arch in zip(roots, ['x86_64', 'arm64']):
                source = root / relative
                run('lipo', '-verify_arch', arch, source)
                thin = destination / ('thin-' + arch)
                arches = output('lipo', '-archs', source).split()
                if len(arches) == 1:
                    shutil.copy2(source, thin)
                else:
                    run('lipo', source, '-thin', arch, '-output', thin)
                thin_files.append(thin)
            run('lipo', '-create', *thin_files, '-output', path)
            path.chmod(0o755)
            run('lipo', '-verify_arch', 'x86_64', 'arm64', path)
        elif path.read_bytes() != counterpart.read_bytes():
            raise RuntimeError('Architecture resources differ: ' + str(relative))
    extra = {p.relative_to(roots[1]) for p in roots[1].rglob('*') if p.is_file() and '_CodeSignature' not in p.parts} - {
        p.relative_to(roots[0]) for p in roots[0].rglob('*') if p.is_file() and '_CodeSignature' not in p.parts}
    if extra:
        raise RuntimeError('ARM-only bundle resources: ' + str(extra))
    sign_and_verify(app)
    art = destination / 'dmg-background.png'
    run('python3', ROOT / 'scripts/packaging/dmg_background.py', '--wave', background, '--output', art)
    artifact = destination / f'OpenXMB-{VERSION}-macos-universal.dmg'
    run('dmgbuild', '-s', ROOT / 'scripts/packaging/dmg_settings.py',
        '-D', 'app=' + str(app.resolve()), '-D', 'background=' + str(art.resolve()), 'OpenXMB', artifact)
    run('hdiutil', 'verify', artifact)
    return artifact


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=['linux', 'windows', 'mac-thin', 'mac-universal'])
    parser.add_argument('--stage', type=Path, default=Path('dist/stage'))
    parser.add_argument('--output', type=Path, default=Path('dist/packages'))
    parser.add_argument('--arch', choices=['x86_64', 'arm64'])
    parser.add_argument('--intel', type=Path)
    parser.add_argument('--arm', type=Path)
    parser.add_argument('--background', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.kind == 'linux':
        artifact = linux(args.stage.resolve(), args.output.resolve())
    elif args.kind == 'windows':
        artifact = windows(args.stage.resolve(), args.output.resolve())
    elif args.kind == 'mac-thin':
        artifact = mac_thin(args.stage.resolve(), args.output.resolve(), args.arch)
    else:
        artifact = universal(args.intel.resolve(), args.arm.resolve(), args.background.resolve(), args.output.resolve())
    artifact.with_name(artifact.name + '.sha256').write_text(hashlib.sha256(artifact.read_bytes()).hexdigest() + '  ' + artifact.name + '\n')
    print(artifact)


if __name__ == '__main__':
    main()
