#!/usr/bin/env python3
"""Build and test the same installed desktop configuration on every runner."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def run(*args):
    subprocess.run([str(value) for value in args], check=True, cwd=ROOT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', required=True)
    parser.add_argument('--build-dir', type=Path, default=Path('build/release'))
    parser.add_argument('--stage', type=Path, default=Path('dist/stage'))
    args = parser.parse_args()
    build = args.build_dir.resolve()
    stage = args.stage.resolve()
    config = [
        'cmake', '-S', ROOT, '-B', build, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_INSTALL_PREFIX={stage}',
        '-DCMAKE_INSTALL_BINDIR=bin', '-DCMAKE_INSTALL_DATAROOTDIR=share',
        '-DAUROREENGINE_LOCAL=', '-DOPENXMB_IMPORT_XMB_WEB_COMPAT=OFF',
        '-DOPENXMB_BUILD_NATIVE_TESTS=ON', '-DAUROREENGINE_BUILD_V2_TESTS=ON',
        '-DENABLE_BROWSER=ON', '-DENABLE_VIDEO_PLAYER=ON',
        '-DENABLE_LIBRETRO=ON', '-DENABLE_DISC_MEDIA=ON',
        '-DINTERFACE_FX_DEBUG=OFF', '-DOPENXMB_ENABLE_NATIVE_OPTIMIZATIONS=OFF',
        '-DOPENXMB_ENABLE_IPO=OFF', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
    ]
    for name in ['CXX', 'CC']:
        if os.environ.get(name):
            config.append(f'-DCMAKE_{"CXX" if name == "CXX" else "C"}_COMPILER={os.environ[name]}')
    for name in ['msgfmt', 'msgmerge']:
        executable = shutil.which(name)
        if not executable:
            raise RuntimeError('Release builds require the gettext catalog tool: ' + name)
        config.append(f'-DGETTEXT_{name.upper()}_EXECUTABLE={executable}')
    if platform.system() == 'Windows':
        prefix = Path(os.environ['VCPKG_INSTALLED_DIR']) / 'x64-windows'
        config += [f'-DCMAKE_TOOLCHAIN_FILE={os.environ["VCPKG_ROOT"]}/scripts/buildsystems/vcpkg.cmake',
                   f'-DVCPKG_INSTALLED_DIR={os.environ["VCPKG_INSTALLED_DIR"]}',
                   f'-DVCPKG_OVERLAY_TRIPLETS={ROOT}/scripts/ci/triplets',
                   '-DVCPKG_TARGET_TRIPLET=x64-windows',
                   f'-DVulkan_INCLUDE_DIR={prefix}/include', f'-DVulkan_LIBRARY={prefix}/lib/vulkan-1.lib']
    elif platform.system() == 'Darwin':
        sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
        config += [f'-DCMAKE_OSX_ARCHITECTURES={args.arch}', '-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0',
                   f'-DCMAKE_OSX_SYSROOT={sdk}']
    else:
        config += ['-DCMAKE_LINKER_TYPE=LLD']
    run(*config)
    run('cmake', '--build', build, '--parallel', os.environ.get('CMAKE_BUILD_PARALLEL_LEVEL', '2'))
    run('ctest', '--test-dir', build, '--output-on-failure', '--no-tests=error', '-j', '1')
    run('cmake', '--install', build)
    stage.mkdir(parents=True, exist_ok=True)
    (stage / 'build-info.json').write_text(json.dumps({
        'version': (ROOT / 'VERSION').read_text().strip(), 'architecture': args.arch,
        'commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
