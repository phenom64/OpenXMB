#!/usr/bin/env python3
"""Find a CPU Vulkan ICD for graphics smoke tests on GPU-less hosted runners.

The software driver is used only in CI and is never included in release assets.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import urllib.request
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--optional', action='store_true')
    args = parser.parse_args()
    system = platform.system()
    if system == 'Linux':
        icd = next(Path('/usr/share/vulkan/icd.d').glob('*lvp*.json'))
    else:
        target = 'win64' if system == 'Windows' else ('mac-arm64' if platform.machine() == 'arm64' else 'mac-x64')
        url = 'https://googlechromelabs.github.io/chrome-for-testing/last-known-good-versions-with-downloads.json'
        with urllib.request.urlopen(url, timeout=60) as response:
            metadata = json.load(response)
        channel = metadata['channels']['Stable']
        download = next(item['url'] for item in channel['downloads']['chrome'] if item['platform'] == target)
        root = Path(os.environ['RUNNER_TEMP']) / 'openxmb-software-vulkan'
        root.mkdir(exist_ok=True)
        archive = root / 'chrome.zip'
        urllib.request.urlretrieve(download, archive)
        with zipfile.ZipFile(archive) as source:
            source.extractall(root)
        candidates = list(root.rglob('vk_swiftshader_icd.json'))
        if not candidates:
            if args.optional:
                print('No software Vulkan ICD is shipped for this architecture; bundle startup and dependency checks remain required.')
                return
            raise RuntimeError('A software Vulkan ICD is required for the Windows graphics smoke')
        icd = candidates[0]
        print('Software Vulkan smoke driver from Chrome for Testing ' + channel['version'])
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
