#!/usr/bin/env python3
"""Headless Settings rescan using only the profile's selected RPCS3 installation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess


def game(folder, title_id, category):
    folder.mkdir(parents=True)
    (folder / 'USRDIR').mkdir()
    (folder / 'USRDIR/EBOOT.BIN').write_bytes(b'non-executable discovery fixture')
    fields = [('TITLE', title_id), ('TITLE_ID', title_id), ('CATEGORY', category)]
    keys, values, rows = bytearray(), bytearray(), bytearray()
    for key, value in fields:
        encoded = value.encode() + b'\0'
        rows += struct.pack('<HHIII', len(keys), 0x0204, len(encoded), len(encoded), len(values))
        keys += key.encode() + b'\0'
        values += encoded
    key_start = 20 + len(rows)
    value_start = (key_start + len(keys) + 3) & ~3
    header = struct.pack('<5I', 0x46535000, 0x101, key_start, value_start, len(fields))
    (folder / 'PARAM.SFO').write_bytes(header + rows + keys + bytes(value_start - key_start - len(keys)) + values)


def hashes(root):
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob('*') if path.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--invalid-executable', action='store_true', help='A moved executable must show a Settings error without exiting')
    args = parser.parse_args()
    app, root = args.app.resolve(), args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    sources = root / 'sources'
    emulator = sources / 'RPCS3 é' / ('rpcs3.exe' if os.name == 'nt' else 'rpcs3')
    emulator.parent.mkdir(parents=True)
    emulator.write_bytes(b'non-executable discovery fixture')
    emulator.chmod(0o755)
    portable = emulator.parent / 'portable'
    disc = sources / 'Disc café/PS3_GAME'
    game(disc, 'BETA00001', 'DG')
    game(portable / 'dev_hdd0/game/BETA00002', 'BETA00002', 'HG')
    game(portable / 'dev_hdd0/game/BETA00003', 'BETA00003', 'GD')
    registry = portable / ('config/games.yml' if os.name == 'nt' else 'games.yml')
    registry.parent.mkdir(parents=True, exist_ok=True)
    registry.write_text(json.dumps('BETA00001') + ': ' + json.dumps(str(disc)) + '\n', encoding='utf-8')
    before = hashes(sources)
    profile = root / 'data/profiles/default'
    profile.mkdir(parents=True)
    (root / 'data/profiles.json').write_text(json.dumps({'version': 1, 'active_profile': 'default',
        'profiles': [{'id': 'default', 'name': 'Discovery Fixture'}]}))
    config = root / 'config.json'
    config.write_text(json.dumps({'shell': {}, 'preferences': {'setup.completed': 'true',
        'startup.intro': 'Off', 'startup.warning': 'Off', 'audio.startup': 'Off',
        'runner.rpcs3.path': str(emulator.with_name('missing-emulator.exe') if args.invalid_executable else emulator)}}))
    env = {key: value for key, value in os.environ.items() if not key.startswith(
        ('RPCS3_', 'OPENXMB_SETUP_', 'OPENXMB_HARNESS_', 'OPENXMB_FIXED_', 'OPENXMB_INITIAL_', 'AUROREENGINE_HEADLESS'))}
    env.update({'OPENXMB_CONFIG': str(config), 'OPENXMB_DATA_DIR': str(root / 'data'),
        'XMB_ASSET_DIR': str(app.parent / 'shell'), 'XMB_LOCALE_DIR': str(app.parent / 'locales'),
        'OPENXMB_FORCE_SETUP': '0', 'SDL_AUDIODRIVER': 'dummy', 'SPDLOG_LEVEL': 'warn',
        'PATH': '', 'OPENXMB_INITIAL_CATEGORY': 'settings',
        'OPENXMB_INITIAL_SETTINGS_MENU': 'category.settings.desktop.Game',
        'OPENXMB_INITIAL_SETTINGS_SELECTION': '1', 'OPENXMB_HARNESS_ACTIONS': '0.25:ok',
        'AUROREENGINE_HEADLESS': '1', 'AUROREENGINE_HEADLESS_WIDTH': '960',
        'AUROREENGINE_HEADLESS_HEIGHT': '540', 'AUROREENGINE_HEADLESS_FRAMES': '48'})
    for name in ('APPDATA', 'LOCALAPPDATA', 'HOME', 'XDG_CONFIG_HOME'):
        folder = root / ('isolated-' + name.lower())
        folder.mkdir()
        env[name] = str(folder)
    for iteration in range(2):
        result = subprocess.run([str(app)], cwd=app.parent, env=env, capture_output=True, timeout=90)
        (root / f'native-{iteration}.log').write_bytes(result.stdout + result.stderr)
        assert result.returncode == 0, 'Settings rescan exited the app: ' + (result.stdout + result.stderr).decode(errors='replace')
        library = profile / 'library.json'
        entries = json.loads(library.read_text(encoding='utf-8'))['games'] if library.exists() else []
        registered = [entry for entry in entries if entry.get('runner') == 'rpcs3']
        if args.invalid_executable:
            assert not registered, 'Invalid selected executable fell back to another installation'
            assert hashes(sources) == before, 'Invalid-path handling changed source files'
            print('PASS moved RPCS3 executable keeps Settings alive and preserves source files')
            return
        assert {entry['title_id'] for entry in registered} == {'BETA00001', 'BETA00002'}, registered
        assert len(registered) == 2, 'Rescan duplicated registered games'
        assert all(Path(entry['executable']).resolve() == emulator for entry in registered), registered
        assert hashes(sources) == before, 'Discovery changed emulator/game source files'
    print('PASS Settings uses the configured RPCS3 path: DG/HG import, GD exclusion, idempotent rescan and source preservation')


if __name__ == '__main__':
    main()
