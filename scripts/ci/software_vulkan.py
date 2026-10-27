#!/usr/bin/env python3
"""Find a CPU Vulkan ICD for graphics smoke tests on GPU-less hosted runners.

The software driver is used only in CI and is never included in release assets.
"""
import argparse
import http.client
import os
from pathlib import Path
import platform
import time
import urllib.error
import urllib.request
import zipfile

CHROME_VERSION = '156.0.8078.47'


def download_archive(url, archive):
    """Retry transient DNS/transport failures without retaining partial archives."""
    temporary = archive.with_suffix('.download')
    for attempt in range(4):
        try:
            with urllib.request.urlopen(url, timeout=45) as response, temporary.open('wb') as output:
                while block := response.read(1024 * 1024):
                    output.write(block)
            with zipfile.ZipFile(temporary) as candidate:
                if candidate.testzip() is not None:
                    raise zipfile.BadZipFile('Corrupt software Vulkan archive')
            temporary.replace(archive)
            return
        except (urllib.error.URLError, TimeoutError, ConnectionError,
                http.client.HTTPException, zipfile.BadZipFile) as error:
            temporary.unlink(missing_ok=True)
            if isinstance(error, urllib.error.HTTPError) and error.code not in (408, 429) and error.code < 500:
                raise
            if attempt == 3:
                raise
            print(f'Software Vulkan download attempt {attempt + 1} failed: {error}; retrying', flush=True)
            time.sleep(2 ** (attempt + 1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--optional', action='store_true')
    args = parser.parse_args()
    system = platform.system()
    if system == 'Linux':
        icd = next(Path('/usr/share/vulkan/icd.d').glob('*lvp*.json'))
    else:
        target = 'win64' if system == 'Windows' else ('mac-arm64' if platform.machine() == 'arm64' else 'mac-x64')
        download = (f'https://storage.googleapis.com/chrome-for-testing-public/{CHROME_VERSION}/'
                    f'{target}/chrome-{target}.zip')
        root = Path(os.environ['RUNNER_TEMP']) / 'openxmb-software-vulkan' / f'{target}-{CHROME_VERSION}'
        root.mkdir(parents=True, exist_ok=True)
        archive = root / 'chrome.zip'
        if archive.is_file() and not zipfile.is_zipfile(archive):
            archive.unlink()
        if not archive.is_file():
            download_archive(download, archive)
        with zipfile.ZipFile(archive) as source:
            source.extractall(root)
        candidates = list(root.rglob('vk_swiftshader_icd.json'))
        if not candidates:
            if args.optional:
                print('No software Vulkan ICD is shipped for this architecture; bundle startup and dependency checks remain required.')
                return
            raise RuntimeError('A software Vulkan ICD is required for the Windows graphics smoke')
        icd = candidates[0]
        print('Software Vulkan smoke driver from Chrome for Testing ' + CHROME_VERSION)
    if system == 'Windows' and os.environ.get('GITHUB_ACTIONS') == 'true':
        # Hosted Windows runners run elevated. The Vulkan loader deliberately
        # ignores environment driver overrides in that case, so register this
        # test-only ICD in the disposable runner's normal driver search path.
        import winreg
        try:
            with winreg.CreateKeyEx(winreg.HKEY_LOCAL_MACHINE,
                    r'SOFTWARE\Khronos\Vulkan\Drivers', 0,
                    winreg.KEY_WRITE | winreg.KEY_WOW64_64KEY) as key:
                winreg.SetValueEx(key, str(icd.resolve()), 0, winreg.REG_DWORD, 0)
            print('Registered software Vulkan ICD for the elevated hosted runner')
        except PermissionError:
            print('Machine-wide ICD registration is unavailable; using the environment override')
    with Path(os.environ['GITHUB_ENV']).open('a') as environment:
        environment.write('VK_ICD_FILENAMES=' + str(icd.resolve()) + '\n'
                          'VK_DRIVER_FILES=' + str(icd.resolve()) + '\n'
                          'VK_LOADER_DEBUG=error,warn,driver\nOPENXMB_SOFTWARE_VULKAN=1\n')


if __name__ == '__main__':
    main()
