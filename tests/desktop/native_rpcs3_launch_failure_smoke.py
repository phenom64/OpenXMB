#!/usr/bin/env python3
"""Verify managed snapshot rollback when the native child cannot be launched."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def inventory(root):
    return {path.relative_to(root).as_posix():
            (hashlib.sha256(path.read_bytes()).hexdigest(), path.stat().st_mtime_ns)
            for path in root.rglob('*') if path.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('This native fixture requires Windows')
    app, root = args.app.resolve(), args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    source = root / 'RPCS3 fixture café'
    for folder in ('config', 'GuiConfigs', 'cache', 'dev_hdd0/home/00000007'):
        (source / folder).mkdir(parents=True, exist_ok=True)
    executable = source / 'rpcs3.exe'
    executable.write_bytes(b'This is an intentionally non-executable RPCS3 fixture.')
    (source / 'RPCS3.buf').write_bytes(b'')
    (source / 'config/config.yml').write_text('Core: preserve exactly\n', encoding='utf-8')
    (source / 'GuiConfigs/CurrentSettings.ini').write_text(
        '[main_window]\nconfirmationBoxExitGame=true\n', encoding='utf-8')
    (source / 'GuiConfigs/persistent_settings.dat').write_text(
        '[Users]\nactive_user=00000007\n', encoding='utf-8')
    (source / 'cache/sentinel').write_bytes(b'Original cache must survive failed launch cleanup.')
    before = inventory(source)
    content = root / 'PS3_GAME/USRDIR/EBOOT.BIN'
    content.parent.mkdir(parents=True)
    content.write_bytes(b'Non-bootable game fixture; no emulator will run.')
    profile = root / 'data/profiles/default'
    profile.mkdir(parents=True)
    (root / 'data/profiles.json').write_text(json.dumps({'version': 1,
        'active_profile': 'default', 'profiles': [{'id': 'default', 'name': 'Launch failure fixture'}]}))
    (profile / 'library.json').write_text(json.dumps({'version': 1, 'games': [{
        'id': 'failed-launch', 'title': '000 Managed launch failure fixture', 'runner': 'rpcs3',
        'platform': 'PlayStation 3', 'content': str(content), 'game_directory': str(content.parent.parent),
        'executable': str(executable)}]}))
    config = root / 'config.json'
    config.write_text(json.dumps({'shell': {}, 'preferences': {'setup.completed': 'true',
        'startup.intro': 'Off', 'startup.warning': 'Off', 'audio.startup': 'Off',
        'runner.rpcs3.path': str(executable)}}))
    env = {key: value for key, value in os.environ.items() if not key.startswith((
        'RPCS3_', 'OPENXMB_', 'AUROREENGINE_HEADLESS'))}
    for variable in ('APPDATA', 'LOCALAPPDATA', 'HOME', 'XDG_CONFIG_HOME'):
        directory = root / variable.lower()
        directory.mkdir()
        env[variable] = str(directory)
    env.update({'OPENXMB_CONFIG': str(config), 'OPENXMB_DATA_DIR': str(root / 'data'),
        'OPENXMB_FORCE_SETUP': '0', 'OPENXMB_INITIAL_CATEGORY': 'game',
        'OPENXMB_HARNESS_ACTIONS': '0.5:ok', 'RPCS3_CONFIG_DIR': str(source) + os.sep,
        'XMB_ASSET_DIR': str(app.parent / 'shell'), 'XMB_LOCALE_DIR': str(app.parent / 'locales'),
        'PATH': '', 'SDL_AUDIODRIVER': 'dummy', 'SPDLOG_LEVEL': 'debug',
        'AUROREENGINE_HEADLESS': '1', 'AUROREENGINE_HEADLESS_WIDTH': '960',
        'AUROREENGINE_HEADLESS_HEIGHT': '540', 'AUROREENGINE_HEADLESS_FRAMES': '120',
        'AUROREENGINE_HEADLESS_OUTPUT_DIR': str(root / 'frames'),
        'AUROREENGINE_HEADLESS_OUTPUT_PATTERN': 'final.png'})
    result = subprocess.run([str(app)], cwd=app.parent, env=env, capture_output=True, timeout=90)
    log = (result.stdout + result.stderr).decode(errors='replace')
    (root / 'native.log').write_text(log, encoding='utf-8')
    assert result.returncode == 0, log
    assert ('RPCS3 managed session reuses its original recompiler cache' in log or
            'RPCS3 managed cache fallback:' in log), 'Fixture never reached managed preparation'
    assert inventory(source) == before, 'Failed launch changed original preferences or cache'
    assert not list(profile.glob('rpcs3-session-*')), 'Failed child launch leaked a private RPCS3 snapshot'
    assert (root / 'frames/final.png').exists(), 'Launch error closed the native shell'
    print('PASS failed native child launch rolls back the private snapshot and preserves original files/cache')


if __name__ == '__main__':
    main()
