#!/usr/bin/env python3
"""Build the Linux x86-64 tarball from a trusted Ubuntu 24.04 CMake install.

Run only on the build machine: ldd executes code from the inspected ELF files.
The resulting browser still requires an actual sandboxed integration test.
"""
import argparse
import hashlib
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ('ffmpeg', 'ffprobe', 'pdfinfo', 'pdftoppm', 'pdftotext')
FONT_RESOURCES = ('NotoSansDevanagari-Regular.ttf', 'NotoSansCJK-Regular.ttc', 'NotoSans-Regular.ttf',
                  'OFL-Noto.txt', 'OFL-NotoCJK.txt', 'COPYRIGHT-Noto.txt', 'COPYRIGHT-NotoCJK.txt')
REQUIRED = ('bin/openxmb-web', 'bin/XMS.bin', 'bin/openxmb-integrations',
            'bin/openxmb-retro', 'share/OpenXMB/config.json',
            'share/shell/Play-Regular.ttf', 'share/shell/catalog/en.json')
GLIBC = re.compile(r'^(?:ld-linux[^/]*|lib(?:c|m|mvec|pthread|dl|rt|resolv|util|anl|BrokenLocale|nss_[^.]*)\.so(?:\..*)?)$')
DRIVER = re.compile(r'^(?:.*_dri\.so|lib(?:GLX|EGL|GLESv[12])_(?:mesa|nvidia).*|'
                    r'libvulkan_(?!loader).*|libnvidia[^/]*|libcuda[^/]*|libnvcuvid[^/]*)$', re.I)


def host_only(name):
    return name == 'libnsl.so.1' or bool(GLIBC.fullmatch(name) or DRIVER.fullmatch(name))


def parse_ldd(text):
    result = {}
    for line in text.splitlines():
        if '=>' not in line:
            continue
        name, value = (part.strip() for part in line.split('=>', 1))
        if value.startswith('not found'):
            raise RuntimeError('Missing ELF dependency: ' + name)
        match = re.match(r'(.+?)\s+\(0x[0-9a-fA-F]+\)\s*$', value)
        if not match or not match.group(1).startswith('/'):
            raise RuntimeError('Unrecognized ldd output: ' + line)
        result[name] = match.group(1)
    return result


def launcher_environment():
    return '''
case "$DIR" in *:*) echo "OpenXMB: paths containing a colon are not supported by the Linux loader" >&2; exit 1;; esac
export PATH="$DIR/bin:$PATH"
host_has_versions() {
    lib=$1
    requirements=$2
    cache=$(command -v ldconfig 2>/dev/null || true)
    [ -n "$cache" ] || [ ! -x /sbin/ldconfig ] || cache=/sbin/ldconfig
    [ -n "$cache" ] || return 1
    candidate=$("$cache" -p 2>/dev/null | while IFS= read -r line; do
        case "$line" in *"$lib "*x86-64*'=> '*) printf '%s\\n' "${line#*=> }"; break;; esac
    done)
    [ -n "$candidate" ] && [ -r "$candidate" ] || return 1
    definitions=$(LC_ALL=C LD_LIBRARY_PATH="$DIR/lib:$DIR/lib/fallback/cxx:$DIR/lib/fallback/gcc" "$DIR/bin/openxmb-readelf" --version-info "$candidate") || return 1
    definitions=$(printf '%s\\n' "$definitions" | sed -n '/^Version definition section/,/^Version needs section/p' | sed -n 's/.*Name: \\([^[:space:]]*\\).*/\\1/p')
    while IFS= read -r need || [ -n "$need" ]; do
        [ -n "$need" ] || continue
        printf '%s\\n' "$definitions" | grep -F -x -q -- "$need" || return 1
    done < "$requirements"
}
LIBPATH="$DIR/lib:$DIR/libexec/webkit:$DIR"
for item in cxx gcc; do
    case "$item" in
        cxx) lib=libstdc++.so.6;;
        gcc) lib=libgcc_s.so.1;;
    esac
    if [ -f "$DIR/lib/fallback/$item/$lib" ]; then
        requirements="$DIR/lib/fallback/$item/required-versions"
        if [ ! -f "$requirements" ]; then
            echo "OpenXMB: missing runtime version requirements: $requirements" >&2; exit 1
        fi
        if ! host_has_versions "$lib" "$requirements"; then
            LIBPATH="$LIBPATH:$DIR/lib/fallback/$item"
            echo "OpenXMB: using bundled $lib; host GPU drivers may need a newer runtime" >&2
        fi
    fi
done
export LD_LIBRARY_PATH="$LIBPATH"
export FONTCONFIG_FILE="$DIR/etc/fonts/fonts.conf"
export GIO_MODULE_DIR="$DIR/lib/gio/modules"
export GSETTINGS_SCHEMA_DIR="$DIR/share/glib-2.0/schemas"
export XDG_DATA_DIRS="$DIR/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export XMB_ASSET_DIR="${XMB_ASSET_DIR:-$DIR/share/shell}"
export XMB_LOCALE_DIR="${XMB_LOCALE_DIR:-$DIR/share/locale}"
# A host ICD may depend on newer display-protocol libraries than the bundle.
# Select only the host providers proved necessary; absent ones use bundled copies.
provider_cache=$(command -v ldconfig 2>/dev/null || true)
[ -n "$provider_cache" ] || [ ! -x /sbin/ldconfig ] || provider_cache=/sbin/ldconfig
providers=${LD_PRELOAD-}
if [ -n "$provider_cache" ]; then
    for provider in libwayland-client.so.0 libxcb-dri3.so.0; do
        candidate=$("$provider_cache" -p 2>/dev/null | while IFS= read -r line; do
            case "$line" in *"$provider "*x86-64*'=> '*) printf '%s\\n' "${line#*=> }"; break;; esac
        done)
        case "$candidate" in
            /*) case "$candidate" in *[[:space:]:]*) continue;; esac;;
            *) continue;;
        esac
        [ -r "$candidate" ] || continue
        case ":$providers:" in *":$candidate:"*) ;; *) providers="$candidate${providers:+:$providers}";; esac
    done
fi
[ -z "$providers" ] || export LD_PRELOAD="$providers"
'''


def launcher_script():
    return '''#!/bin/sh
set -eu
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
''' + launcher_environment() + '''
exec "$DIR/bin/XMS.bin" "$@"
'''


def browser_launcher_script():
    return '''#!/bin/sh
set -eu
DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
''' + launcher_environment() + '''
if [ "$#" -eq 4 ]; then
    profile=$3
    state=$4
    case "$profile" in /*) ;; *) profile="$PWD/$profile";; esac
    case "$state" in /*) ;; *) state="$PWD/$state";; esac
    set -- "$1" "$2" "$profile" "$state"
fi
cd "$DIR"
exec "$DIR/bin/openxmb-web.bin" "$@"
'''


def portable_executor_script():
    return '''#!/bin/sh
set -eu
DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
''' + launcher_environment() + '''
exec "$@"
'''


def validate_stage(stage):
    for name in REQUIRED:
        if not (stage / name).is_file():
            raise RuntimeError('Missing staged resource: ' + name)
    if not (stage / 'share/locale').is_dir():
        raise RuntimeError('Missing staged resource: share/locale')
    for name in FONT_RESOURCES:
        path = stage / 'share/shell/fonts' / name
        if not path.is_file() or path.stat().st_size == 0:
            raise RuntimeError('Missing or empty staged font resource: share/shell/fonts/' + name)


def relocate_webkit_bytes(data, original):
    replacement = b'./libexec/webkit'
    pattern = original + b'\0'
    if len(replacement) > len(original) or data.count(pattern) != 1:
        raise RuntimeError('Cannot relocate WebKit compiled-in helper path unambiguously')
    return data.replace(pattern, replacement + b'\0' * (len(pattern) - len(replacement)), 1)


def relocate_webkit_injected_bundle(app, original):
    """Rewrite each exact compiled-in injected-bundle directory across packaged ELFs."""
    pattern = original + b'/injected-bundle/\0'
    replacement = b'./libexec/webkit/injected-bundle/'
    if len(replacement) >= len(pattern):
        raise RuntimeError('Cannot relocate WebKit injected-bundle path without changing ELF size')
    matches = []
    for binary in app.rglob('*'):
        if is_elf(binary):
            data = binary.read_bytes()
            count = data.count(pattern)
            if count:
                matches.append((binary, data, count))
    if not matches:
        raise RuntimeError('Cannot locate exact WebKit injected-bundle path')
    replacement = replacement + b'\0' * (len(pattern) - len(replacement))
    for binary, data, _ in matches:
        binary.write_bytes(data.replace(pattern, replacement))


def fontconfig_configuration():
    return '''<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <include ignore_missing="yes">/etc/fonts/fonts.conf</include>
  <dir prefix="relative">../../share/shell/fonts</dir>
  <cachedir prefix="xdg">fontconfig</cachedir>
</fontconfig>
'''


def command(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True, stderr=subprocess.STDOUT)


def is_elf(path):
    if not path.is_file() or path.is_symlink():
        return False
    with path.open('rb') as stream:
        return stream.read(4) == b'\x7fELF'


def executable(path, contents):
    path.write_text(contents, encoding='utf-8', newline='\n')
    path.chmod(0o755)


def copy_portable_notes(app):
    shutil.copy2(ROOT / 'docs/linux-portable.md', app / 'PORTABLE.md')


def collect_elf_dependencies(app):
    """Resolve each trusted ELF's dependency graph and flatten by SONAME."""
    libdir = app / 'lib'
    libdir.mkdir(exist_ok=True)
    seen = {}
    for existing in libdir.iterdir():
        if existing.is_file() and is_elf(existing.resolve()):
            seen[existing.name] = hashlib.sha256(existing.read_bytes()).hexdigest()
    queue = [p for p in app.rglob('*') if is_elf(p)]
    sources = set()
    while queue:
        binary = queue.pop()
        try:
            deps = parse_ldd(command('ldd', binary))
        except (subprocess.CalledProcessError, RuntimeError) as error:
            raise RuntimeError(f'Cannot inspect {binary}: {error}') from error
        for name, filename in deps.items():
            if host_only(name) or host_only(Path(filename).name):
                continue
            source = Path(filename).resolve(strict=True)
            if not is_elf(source):
                raise RuntimeError(f'Not an ELF dependency: {source}')
            digest = hashlib.sha256(source.read_bytes()).hexdigest()
            for alias in {name, source.name}:
                if '/' in alias or alias in ('.', '..'):
                    raise RuntimeError('Unsafe dependency name: ' + alias)
                if alias in seen and seen[alias] != digest:
                    raise RuntimeError(f'Conflicting ELF dependency {alias}: {source}')
                seen[alias] = digest
            target = libdir / source.name
            if not target.exists():
                shutil.copy2(source, target)
                sources.add(source)
                queue.append(source)
            alias = libdir / name
            if alias != target and not alias.exists():
                alias.symlink_to(target.name)
    return sources


def copy_gles_client(app, source=Path('/usr/lib/x86_64-linux-gnu/libGLESv2.so.2')):
    """Bundle the dlopen-only GLVND client, leaving the host's GPU drivers on the host."""
    if not source.is_file():
        raise RuntimeError('Missing GLES client: ' + str(source))
    source = source.resolve(strict=True)
    if not is_elf(source):
        raise RuntimeError('GLES client is not an ELF: ' + str(source))
    libdir = app / 'lib'
    libdir.mkdir(exist_ok=True)
    target = libdir / source.name
    if target.exists() and hashlib.sha256(target.read_bytes()).digest() != hashlib.sha256(source.read_bytes()).digest():
        raise RuntimeError('Conflicting GLES client: ' + str(target))
    if not target.exists():
        shutil.copy2(source, target)
    alias = libdir / 'libGLESv2.so.2'
    if alias != target:
        if alias.is_symlink() and not alias.exists():
            raise RuntimeError('Broken GLES client alias: ' + str(alias))
        if alias.exists():
            if hashlib.sha256(alias.read_bytes()).digest() != hashlib.sha256(source.read_bytes()).digest():
                raise RuntimeError('Conflicting GLES client alias: ' + str(alias))
        else:
            alias.symlink_to(target.name)
    return source


def copy_copyright(sources, destination):
    """Collect available Debian package copyright files for copied binaries."""
    destination.mkdir(parents=True, exist_ok=True)
    packages = set()
    for source in sources:
        candidates = [source]
        for merged in ('/usr/lib/', '/usr/bin/'):
            if str(source).startswith(merged):
                candidates.append(Path(str(source).removeprefix('/usr')))
        for candidate in candidates:
            try:
                lines = command('dpkg-query', '-S', candidate).splitlines()
            except subprocess.CalledProcessError:
                continue
            for line in lines:
                if ': ' in line:
                    packages.update(x.strip().split(':')[0] for x in line.split(': ', 1)[0].split(','))
            break
    for package in sorted(packages):
        copyright_file = Path('/usr/share/doc') / package / 'copyright'
        if copyright_file.is_file():
            shutil.copy2(copyright_file, destination / (package + '.copyright'))


def prepare_fallbacks(app):
    """Put libstdc++ and libgcc last, after a compatible host runtime."""
    elf_files = [p for p in app.rglob('*') if is_elf(p)]
    versions = {'cxx': set(), 'gcc': set()}
    for binary in elf_files:
        info = command('readelf', '--version-info', binary)
        in_needs = False
        current = None
        for line in info.splitlines():
            if line.startswith('Version needs section'):
                in_needs = True
                current = None
                continue
            if line.startswith('Version ') and ' section ' in line:
                in_needs = False
                current = None
            if not in_needs:
                continue
            library = re.search(r'\bFile:\s*(\S+)', line)
            if library:
                current = {'libstdc++.so.6': 'cxx', 'libgcc_s.so.1': 'gcc'}.get(library.group(1))
            node = re.search(r'\bName:\s*(\S+)', line)
            self_provided = ((current == 'cxx' and binary.name.startswith('libstdc++.so'))
                             or (current == 'gcc' and binary.name.startswith('libgcc_s.so')))
            if current and node and not self_provided:
                versions[current].add(node.group(1))
        for glibc in re.findall(r'\bGLIBC_(\d+)\.(\d+)', info):
            if tuple(map(int, glibc)) > (2, 39):
                raise RuntimeError(f'{binary} requires GLIBC_{glibc[0]}.{glibc[1]} above the 2.39 baseline')
    for item, prefix in (('cxx', 'libstdc++.so'), ('gcc', 'libgcc_s.so')):
        files = list((app / 'lib').glob(prefix + '*'))
        if not files:
            raise RuntimeError('Missing bundled fallback: ' + prefix)
        directory = app / 'lib/fallback' / item
        directory.mkdir(parents=True)
        for path in files:
            path.rename(directory / path.name)
        (directory / 'required-versions').write_text(''.join(name + '\n' for name in sorted(versions[item])), encoding='ascii')


def webkit_helpers(app):
    libraries = list((app / 'lib').glob('libwebkit2gtk-4.1.so.*'))
    libraries = [p for p in libraries if not p.is_symlink()]
    if len(libraries) != 1:
        raise RuntimeError('Expected exactly one bundled WebKitGTK 4.1 library')
    candidates = [Path('/usr/lib/x86_64-linux-gnu/webkit2gtk-4.1'),
                  Path('/usr/libexec/webkit2gtk-4.1'), Path('/usr/lib/webkit2gtk-4.1')]
    paths = [p for p in candidates if (p / 'WebKitWebProcess').is_file()
             and (p / 'WebKitNetworkProcess').is_file()]
    if len(paths) != 1:
        raise RuntimeError('Expected one WebKitGTK directory with WebProcess and NetworkProcess')
    helper_dir = paths[0]
    target = app / 'libexec/webkit'
    shutil.copytree(helper_dir, target, symlinks=False)
    return libraries[0], helper_dir


def resolve_gio_querymodules(fallback=Path('/usr/lib/x86_64-linux-gnu/glib-2.0/gio-querymodules')):
    program = shutil.which('gio-querymodules')
    if program:
        return program
    if fallback.is_file() and os.access(fallback, os.X_OK):
        return str(fallback)
    raise RuntimeError('Missing build dependency: gio-querymodules (PATH or ' + str(fallback) + ')')


def copy_gio_modules(app):
    source = Path('/usr/lib/x86_64-linux-gnu/gio/modules')
    if not (source / 'libgiognutls.so').is_file():
        raise RuntimeError('Missing GIO TLS backend; install glib-networking')
    target = app / 'lib/gio/modules'
    shutil.copytree(source, target, symlinks=False)
    subprocess.run([resolve_gio_querymodules(), str(target)], check=True)


def apparmor_profile_script():
    return r'''#!/bin/sh
set -eu
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
path="$DIR/bin/openxmb-web.bin"
# AppArmor syntax has glob and quoting metacharacters. Reject them instead of
# generating a profile that accidentally applies to a different executable.
case "$path" in
    *'"'*|*'\'*|*'*'*|*'?'*|*'['*|*']'*|*'{'*|*'}'*|*'
'*) echo "Unsupported profile path; move the archive to a path without AppArmor metacharacters" >&2; exit 1;;
esac
id=$(printf '%s' "$path" | sha256sum | cut -c1-16)
printf 'abi <abi/4.0>,\nprofile openxmb-portable-%s "%s" flags=(unconfined) {\n  userns,\n}\n' "$id" "$path"
'''


def build(stage, output, version):
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'amd64'):
        raise RuntimeError('Build this archive on Linux x86-64, using Ubuntu 24.04')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.+_-]*', version):
        raise RuntimeError('Invalid version')
    validate_stage(stage)
    for program in ('ldd', 'readelf', 'patchelf', 'dpkg-query', *TOOLS):
        if not shutil.which(program):
            raise RuntimeError('Missing build dependency: ' + program)
    if platform.libc_ver()[0] != 'glibc' or platform.libc_ver()[1] != '2.39':
        raise RuntimeError('Build on the Ubuntu 24.04 glibc 2.39 baseline')
    output.mkdir(parents=True, exist_ok=True)
    name = f'OpenXMB-{version}-linux-x86_64'
    artifact = output / (name + '.tar.gz')
    with tempfile.TemporaryDirectory(prefix='openxmb-portable-') as temporary:
        app = Path(temporary) / name
        shutil.copytree(stage, app, symlinks=False)
        copy_portable_notes(app)
        fontconfig = app / 'etc/fonts/fonts.conf'
        fontconfig.parent.mkdir(parents=True, exist_ok=True)
        fontconfig.write_text(fontconfig_configuration(), encoding='utf-8', newline='\n')
        for binary in (p for p in app.rglob('*') if is_elf(p)):
            if host_only(binary.name):
                raise RuntimeError('The stage contains a host-only library or driver: ' + str(binary))
        schemas = Path('/usr/share/glib-2.0/schemas')
        if not (schemas / 'gschemas.compiled').is_file():
            raise RuntimeError('Missing compiled GSettings schemas')
        shutil.copytree(schemas, app / 'share/glib-2.0/schemas', dirs_exist_ok=True)
        for tool in TOOLS:
            shutil.copy2(Path(shutil.which(tool)).resolve(), app / 'bin' / tool)
        shutil.copy2(Path(shutil.which('readelf')).resolve(), app / 'bin/openxmb-readelf')
        copy_gio_modules(app)
        executable(app / 'OpenXMB', launcher_script())
        (app / 'libexec').mkdir(exist_ok=True)
        executable(app / 'libexec/openxmb-exec', portable_executor_script())
        executable(app / 'print-apparmor-profile', apparmor_profile_script())
        # The browser changes only its own working directory so WebKit's compiled-in
        # relative subprocess directory resolves. XMS retains the caller's cwd.
        browser = app / 'bin/openxmb-web'
        browser.rename(app / 'bin/openxmb-web.bin')
        executable(browser, browser_launcher_script())
        sources = collect_elf_dependencies(app)
        sources.add(copy_gles_client(app))
        webkit_lib, webkit_dir = webkit_helpers(app)
        sources |= collect_elf_dependencies(app)
        webkit_lib.write_bytes(relocate_webkit_bytes(webkit_lib.read_bytes(), os.fsencode(str(webkit_dir))))
        relocate_webkit_injected_bundle(app, os.fsencode(str(webkit_dir)))
        prepare_fallbacks(app)
        # Every packaged ELF gets a location-independent RUNPATH. LD_LIBRARY_PATH
        # also covers plugin processes and is inherited by packaged child tools.
        for binary in (p for p in app.rglob('*') if is_elf(p)):
            relative = os.path.relpath(app / 'lib', binary.parent).replace(os.sep, '/')
            subprocess.run(['patchelf', '--set-rpath', '$ORIGIN/' + relative, str(binary)], check=True)
        notices = app / 'share/doc/openxmb'
        notices.mkdir(parents=True, exist_ok=True)
        for filename in ('LICENSE', 'LICENSE.RetroArch'):
            shutil.copy2(ROOT / filename, notices / filename)
        shutil.copytree(ROOT / 'LICENSES', notices / 'LICENSES', dirs_exist_ok=True)
        copyright_sources = (sources | {Path(shutil.which(t)).resolve() for t in (*TOOLS, 'readelf')}
                             | set(Path('/usr/lib/x86_64-linux-gnu/gio/modules').glob('*.so'))
                             | {p for p in webkit_dir.rglob('*') if p.is_file()})
        copy_copyright(copyright_sources, notices / 'dependencies')
        temporary_archive = Path(temporary) / artifact.name
        with tarfile.open(temporary_archive, 'w:gz', format=tarfile.PAX_FORMAT) as archive:
            archive.add(app, arcname=name)
        shutil.copy2(temporary_archive, artifact)
    hasher = hashlib.sha256()
    with artifact.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            hasher.update(chunk)
    digest = hasher.hexdigest()
    (output / (artifact.name + '.sha256')).write_text(f'{digest}  {artifact.name}\n', encoding='ascii')
    print(artifact)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--version', required=True)
    args = parser.parse_args()
    try:
        build(args.stage.resolve(), args.output.resolve(), args.version)
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print('portable packaging failed: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
