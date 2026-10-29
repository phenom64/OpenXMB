#!/usr/bin/env python3
"""Native provider navigation with isolated RPCS3 fixtures and no live accounts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    app, output = args.app.resolve(), args.output.resolve()
    with tempfile.TemporaryDirectory(prefix='openxmb-providers-') as temporary:
        root = Path(temporary)
        data = root / 'data'
        data.mkdir()
        rpc = root / 'RPCS3/dev_hdd0'
        trophies = rpc / 'home/00000001/trophy/NPWR00001_00'
        trophies.mkdir(parents=True)
        xml = ('<?xml version="1.0"?><trophyconf><title-name>Native Trophy Fixture</title-name>'
               '<trophy id="0" ttype="B" hidden="no"><name>First Fixture</name><detail>Recorded local unlock.</detail></trophy>'
               '<trophy id="1" ttype="G" hidden="yes"><name>Hidden Fixture</name><detail>Concealed fixture detail.</detail></trophy></trophyconf>')
        (trophies / 'TROPCONF.SFM').write_text(xml, encoding='utf-8')
        binary = bytearray(0x210)
        def u32(at, value):
            struct.pack_into('>I', binary, at, value)
        def u64(at, value):
            struct.pack_into('>Q', binary, at, value)
        for at, value in ((0, 0x818f54ad), (8, 2), (0x30, 4), (0x34, 0x50), (0x3c, 2),
                          (0x50, 6), (0x54, 0x60), (0x5c, 2)):
            u32(at, value)
        u64(0x40, 0x70)
        u64(0x60, 0x130)
        for index in range(2):
            entry = 0x70 + index * 0x60
            for at, value in ((entry, 4), (entry + 4, 0x50), (entry + 8, index),
                              (entry + 16, index), (entry + 20, 4 if index == 0 else 2)):
                u32(at, value)
            state = 0x130 + index * 0x70
            for at, value in ((state, 6), (state + 4, 0x60), (state + 8, index),
                              (state + 16, index), (state + 20, 1 if index == 0 else 0)):
                u32(at, value)
            if index == 0:
                u64(state + 40, 62135596800000000 + 1700000000 * 1000000)
        (trophies / 'TROPUSR.DAT').write_bytes(binary)
        for name in ('ICON0.PNG', 'TROP000.PNG', 'TROP001.PNG'):
            shutil.copyfile(app.parent / 'shell/icons/theme/icon_trophy.png', trophies / name)
        before = {path.name: hashlib.sha256(path.read_bytes()).digest() for path in trophies.iterdir()}
        profile = data / 'profiles/default'
        profile.mkdir(parents=True)
        (data / 'profiles.json').write_text(json.dumps({'version': 1, 'active_profile': 'default',
            'profiles': [{'id': 'default', 'name': 'Provider Fixture'}]}), encoding='utf-8')
        (profile / 'library.json').write_text(json.dumps({'version': 1, 'games': [{
            'id': 'steam-settings-fixture', 'title': 'Steam Launch Fixture', 'runner': 'steam',
            'content': '1234567890', 'platform': 'Steam'}]}), encoding='utf-8')
        config = root / 'config.json'
        config.write_text(json.dumps({'shell': {}, 'preferences': {
            'setup.completed': 'true', 'trophies.rpcs3.root': str(rpc),
            'trophies.rpcs3.user': '00000001', 'youtube.executable': str(root / 'missing-youtube.exe'),
        }}), encoding='utf-8')
        env = {key: value for key, value in os.environ.items() if not key.startswith((
            'OPENXMB_SETUP_', 'OPENXMB_HARNESS_', 'OPENXMB_FIXED_', 'OPENXMB_INITIAL_', 'AUROREENGINE_HEADLESS_'))}
        env.update({'OPENXMB_CONFIG': str(config), 'OPENXMB_DATA_DIR': str(data),
                    'XMB_ASSET_DIR': str(app.parent / 'shell'), 'XMB_LOCALE_DIR': str(app.parent / 'locales'),
                    'OPENXMB_FORCE_SETUP': '0', 'SDL_AUDIODRIVER': 'dummy', 'SPDLOG_LEVEL': 'warn',
                    'AUROREENGINE_HEADLESS': '1', 'AUROREENGINE_HEADLESS_WIDTH': '1280',
                    'AUROREENGINE_HEADLESS_HEIGHT': '720'})
        for name, category, actions, frames in (
            ('trophy-collection', 'psn', '0:down;0:ok', 48),
            ('trophy-detail', 'psn', '0:down;0:ok;0.4:ok', 64),
            ('youtube-setup-prompt', 'tv', '0:ok', 24),
            ('youtube-information', 'tv', '0:options;0:down;0:ok', 32),
            ('steam-launch-settings', 'game', '0:options;0:down;0:down;0:down;0:down;0:ok', 32),
        ):
            target = output / name
            target.mkdir(parents=True, exist_ok=True)
            current = env | {'OPENXMB_INITIAL_CATEGORY': category, 'OPENXMB_HARNESS_ACTIONS': actions,
                             'AUROREENGINE_HEADLESS_FRAMES': str(frames),
                             'AUROREENGINE_HEADLESS_OUTPUT_DIR': str(target),
                             'AUROREENGINE_HEADLESS_OUTPUT_PATTERN': 'frame-{:03d}.png'}
            result = subprocess.run([str(app)], cwd=app.parent, env=current, capture_output=True,
                                    encoding='utf-8', errors='replace', timeout=90)
            assert result.returncode == 0, result.stdout + result.stderr
            assert list(target.glob('*.png')), f'{name}: no native frames captured'
            assert not (data / 'applications').exists(), 'A navigation fixture installed an application'
            assert before == {path.name: hashlib.sha256(path.read_bytes()).digest() for path in trophies.iterdir()}, 'Provider modified RPCS3 files'
            print(f'PASS native provider navigation and source preservation: {name}')


if __name__ == '__main__':
    main()
