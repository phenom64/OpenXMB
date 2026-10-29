#!/usr/bin/env python3
"""Exercise the real Trophy Collection matching action with private offline data."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    app, output = args.app.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    for name, age, cache_account, expected in (
        ('fresh', 0, 'user-FixtureUser', '7,14402'),
        ('stale', 172800, 'user-FixtureUser', '7,14402'),
        ('other-account', 0, 'user-OtherUser', '7'),
        ('missing-cache', 0, None, '7'),
    ):
        root = output / name
        profile = root / 'data/profiles/default'
        other_profile = root / 'data/profiles/other'
        profile.mkdir(parents=True)
        other_profile.mkdir()
        other_config = other_profile / 'config.json'
        other_config.write_text(json.dumps({'shell': {}, 'preferences': {
            'achievements.ra.username': 'OtherUser', 'achievements.ra.games': '99'}}), encoding='utf-8')
        other_before = other_config.read_bytes()
        (root / 'data/profiles.json').write_text(json.dumps({
            'version': 1, 'active_profile': 'default', 'profiles': [
                {'id': 'default', 'name': 'Matching Fixture'},
                {'id': 'other', 'name': 'Untouched Fixture'},
            ]}), encoding='utf-8')
        rom = root / 'Fixture café.gba'
        rom.write_bytes(bytes(index % 251 for index in range(70000)))
        rom_before = (hashlib.sha256(rom.read_bytes()).digest(), rom.stat().st_mtime_ns)
        (profile / 'library.json').write_text(json.dumps({'version': 1, 'games': [{
            'id': 'matching-fixture', 'title': 'Generated ROM Fixture', 'runner': 'retroarch',
            'content': str(rom), 'platform': 'Game Boy Advance'}, {
            'id': 'unsupported-fixture', 'title': 'Unsupported Fixture', 'runner': 'steam',
            'content': '1234567890', 'platform': 'Steam'},
        ]}), encoding='utf-8')
        config = profile / 'config.json'
        config.write_text(json.dumps({'shell': {}, 'preferences': {
            'setup.completed': 'true', 'startup.intro': 'Off', 'startup.warning': 'Off',
            'audio.startup': 'Off', 'trophies.rpcs3.root': str(root / 'no-rpcs3'),
            'achievements.ra.username': 'FixtureUser', 'achievements.ra.games': '7',
            'achievements.ra.key_env': 'OPENXMB_RA_MATCH_TEST_UNUSED_KEY',
        }}), encoding='utf-8')
        cache = root / 'data/cache/achievements/default/retroachievements/user-FixtureUser/hashes-5.json'
        if cache_account:
            cache.parent.mkdir(parents=True)
            cache.write_text(json.dumps({'version': 1, 'account': cache_account, 'console': 5,
                'fetched_at': int(time.time()) - age,
                'hashes': {hashlib.md5(rom.read_bytes()).hexdigest(): 14402}}), encoding='utf-8')
        cache_before = cache.read_bytes() if cache.exists() else None
        env = {key: value for key, value in os.environ.items() if not key.startswith((
            'RPCS3_', 'OPENXMB_SETUP_', 'OPENXMB_HARNESS_', 'OPENXMB_FIXED_',
            'OPENXMB_INITIAL_', 'AUROREENGINE_HEADLESS', 'OPENXMB_RA_MATCH_TEST_',
            'RETROACHIEVEMENTS_', 'STEAMGRIDDB_'))}
        for variable in ('APPDATA', 'LOCALAPPDATA', 'HOME', 'XDG_CONFIG_HOME'):
            directory = root / variable.lower()
            directory.mkdir()
            env[variable] = str(directory)
        env.update({
            'OPENXMB_CONFIG': str(config), 'OPENXMB_DATA_DIR': str(root / 'data'),
            'XMB_ASSET_DIR': str(app.parent / 'shell'), 'XMB_LOCALE_DIR': str(app.parent / 'locales'),
            'OPENXMB_FORCE_SETUP': '0', 'OPENXMB_CONFIG_READ_ONLY': '0',
            'SDL_AUDIODRIVER': 'dummy', 'SPDLOG_LEVEL': 'debug',
            'OPENXMB_INITIAL_CATEGORY': 'psn',
            # Open Trophy Collection, select Settings below the manual game ID,
            # wrap Up to Match library games and activate the real action.
            'OPENXMB_HARNESS_ACTIONS': '0:down;0:ok;0.7:down;0.9:ok;1.1:up;1.3:ok',
            'AUROREENGINE_HEADLESS': '1', 'AUROREENGINE_HEADLESS_WIDTH': '1280',
            'AUROREENGINE_HEADLESS_HEIGHT': '720', 'AUROREENGINE_HEADLESS_FRAMES': '240',
            'AUROREENGINE_HEADLESS_OUTPUT_DIR': str(root / 'frames'),
            'AUROREENGINE_HEADLESS_OUTPUT_PATTERN': 'final.png',
        })
        result = subprocess.run([str(app)], cwd=app.parent, env=env, capture_output=True, timeout=90)
        (root / 'native.log').write_bytes(result.stdout + result.stderr)
        assert result.returncode == 0, (result.stdout + result.stderr).decode(errors='replace')
        saved = json.loads(config.read_text(encoding='utf-8'))['preferences']
        assert saved['achievements.ra.games'] == expected, (name, saved['achievements.ra.games'], expected)
        assert saved['achievements.ra.username'] == 'FixtureUser', 'Matching replaced the linked account'
        assert other_config.read_bytes() == other_before, 'Matching modified another profile'
        assert (hashlib.sha256(rom.read_bytes()).digest(), rom.stat().st_mtime_ns) == rom_before, 'Matching modified the ROM'
        assert (cache.read_bytes() if cache.exists() else None) == cache_before, 'Offline matching rewrote the cache'
        assert (root / 'frames/final.png').is_file(), 'Native matching action produced no frame'
        print(f'PASS native offline matching: {name}; manual IDs, account, other profile, ROM and cache preserved')


if __name__ == '__main__':
    main()
