#!/usr/bin/env python3
"""Provision pinned Noto fonts and licenses for release builds."""
import argparse
import hashlib
import os
from pathlib import Path
import platform
import shutil
import tempfile
import time
import urllib.request


NOTO = 'https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/'
CJK = 'https://raw.githubusercontent.com/notofonts/noto-cjk/f8d157532fbfaeda587e826d4cd5b21a49186f7c/'
FILES = (
    ('NotoSansDevanagari-Regular.ttf', NOTO + 'hinted/ttf/NotoSansDevanagari/NotoSansDevanagari-Regular.ttf',
     '385e78e6359a9d88a0f243d53b1209d7548361ba2194e2b9ec779bcaa7e8949d'),
    ('NotoSans-Regular.ttf', NOTO + 'hinted/ttf/NotoSans/NotoSans-Regular.ttf',
     'b85c38ecea8a7cfb39c24e395a4007474fa5a4fc864f6ee33309eb4948d232d5'),
    ('NotoSansCJK-Regular.ttc', CJK + 'Sans/OTC/NotoSansCJK-Regular.ttc',
     'b76b0433203017ca80401b2ee0dd69350349871c4b19d504c34dbdd80541690a'),
    ('OFL-Noto.txt', NOTO + 'LICENSE',
     '0dab92d0544f7b233403f14b84a663bdbfa746982eda629e7f4f9ffe1b036feb'),
    ('OFL-NotoCJK.txt', CJK + 'Sans/LICENSE',
     '6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2'),
)


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def fetch(cache, name, url, expected):
    target = cache / name
    if target.is_file() and sha256(target) == expected:
        return target
    for attempt in range(3):
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=cache, prefix='.download-', delete=False) as output:
                temporary = Path(output.name)
                with urllib.request.urlopen(url, timeout=120) as response:
                    shutil.copyfileobj(response, output, length=1024 * 1024)
            if sha256(temporary) != expected:
                raise RuntimeError('SHA-256 mismatch for ' + name)
            os.replace(temporary, target)
            return target
        except OSError as error:
            if attempt == 2:
                raise RuntimeError(f'Failed to download {name} after 3 attempts: {error}') from error
            time.sleep(attempt + 1)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    raise RuntimeError('Failed to download ' + name)


def provision(cache, destinations, files=FILES):
    cache.mkdir(parents=True, exist_ok=True)
    # Verify every input before publishing any of them to the runtime trees.
    verified = [(name, fetch(cache, name, url, digest), digest) for name, url, digest in files]
    for directory in destinations:
        directory.mkdir(parents=True, exist_ok=True)
        for name, source, digest in verified:
            target = directory / name
            if not target.is_file() or sha256(target) != digest:
                temporary = None
                try:
                    with tempfile.NamedTemporaryFile(dir=directory, prefix='.font-', delete=False) as output:
                        temporary = Path(output.name)
                        with source.open('rb') as content:
                            shutil.copyfileobj(content, output, length=1024 * 1024)
                    os.replace(temporary, target)
                finally:
                    if temporary is not None:
                        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--stage', type=Path)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    # Both build-tree layouts are used by CTest and direct development runs.
    destinations = [build / 'shell/fonts', build / 'share/shell/fonts']
    if args.stage:
        shell = Path('shell') if platform.system() == 'Windows' else Path('share/shell')
        destinations.append(args.stage.resolve() / shell / 'fonts')
    provision(build / 'private/font-cache', destinations)
    # The upstream CJK license has no copyright line; the font's name table
    # carries this separate notice. Keep it alongside the unmodified license.
    for directory in destinations:
        (directory / 'COPYRIGHT-Noto.txt').write_text(
            'Noto Sans Devanagari: Copyright 2015-2022 Google Inc. All Rights Reserved.\n'
            'Noto Sans: Copyright 2015-2021 Google LLC. All Rights Reserved.\n', encoding='utf-8')
        (directory / 'COPYRIGHT-NotoCJK.txt').write_text(
            'Copyright 2014-2021 Adobe (http://www.adobe.com/).\n'
            'Noto is a trademark of Google Inc.\n', encoding='utf-8')
    print('Provisioned pinned Noto fonts and OFL notices', flush=True)


if __name__ == '__main__':
    main()
