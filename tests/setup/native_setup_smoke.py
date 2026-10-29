#!/usr/bin/env python3
"""Exercise real shell setup, checked save and reset using isolated profiles."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    app = args.app.resolve()
    build = app.parent
    with tempfile.TemporaryDirectory(prefix='openxmb-setup-') as temporary:
        root = Path(temporary)
        profile = root / 'profile.json'
        media = root / 'media'
        media.mkdir()
        sentinel = media / 'preserve.bin'
        sentinel.write_bytes(b'Unrelated personal media must survive setup and reset.\x00\xff')
        expected_hash = hashlib.sha256(sentinel.read_bytes()).digest()
        data = root / 'data'
        env = os.environ.copy()
        for key in list(env):
            if key.startswith(('OPENXMB_SETUP_', 'OPENXMB_HARNESS_', 'OPENXMB_FIXED_', 'OPENXMB_INITIAL_', 'AUROREENGINE_HEADLESS_')):
                env.pop(key)
        env.pop('OPENXMB_CONFIG_READ_ONLY', None)
        env.update({
            'OPENXMB_CONFIG': str(profile), 'OPENXMB_DATA_DIR': str(data),
            'XMB_ASSET_DIR': str(build / 'shell'), 'XMB_LOCALE_DIR': str(build / 'locales'),
            'AUROREENGINE_HEADLESS': '1', 'AUROREENGINE_HEADLESS_WIDTH': '1280',
            'AUROREENGINE_HEADLESS_HEIGHT': '720', 'AUROREENGINE_HEADLESS_FRAMES': '4',
            'SDL_AUDIODRIVER': 'dummy', 'SPDLOG_LEVEL': 'warn',
        })
        # Two intro confirmations, a live language change, four page advances,
        # then explicit Yes. This also exercises font/menu reload before save.
        finish = '0:ok;0:ok;0:down;0:right;0:right;0:right;0:right;0:ok;0:left;0:ok'

        def run(name, actions, force=False, read_only=False):
            current = env.copy()
            current['OPENXMB_HARNESS_ACTIONS'] = actions
            current['OPENXMB_FORCE_SETUP'] = '1' if force else '0'
            if name == 'missing-profile-switch':
                current['AUROREENGINE_HEADLESS_FRAMES'] = '16'
            if name == 'reset-button':
                current.update({
                    'OPENXMB_INITIAL_CATEGORY': 'settings',
                    'OPENXMB_INITIAL_SETTINGS_MENU': 'category.settings.desktop.System',
                    'OPENXMB_INITIAL_SETTINGS_SELECTION': '10',
                    'AUROREENGINE_HEADLESS_FRAMES': '100000',
                })
                # Confirmation feedback uses real monotonic time. End through
                # the normal event loop after it has elapsed, not a GPU-speed-
                # dependent number of frames.
                current['OPENXMB_HARNESS_ACTIONS'] = actions + ';0.5:quit'
            if read_only:
                current['OPENXMB_CONFIG_READ_ONLY'] = '1'
            if args.output:
                output = args.output.resolve() / name
                output.mkdir(parents=True, exist_ok=True)
                current['AUROREENGINE_HEADLESS_OUTPUT_DIR'] = str(output)
                current['AUROREENGINE_HEADLESS_OUTPUT_PATTERN'] = 'frame-{:03d}.png'
            result = subprocess.run([str(app), '--width', '1280', '--height', '720',
                                     '--no-fullscreen', '--force-dir', str(media)],
                                    cwd=build, env=current, capture_output=True, text=True,
                                    encoding='utf-8', errors='replace', timeout=90)
            if result.returncode:
                raise AssertionError(f'{name}: exit {result.returncode}\n{result.stdout}\n{result.stderr}')
            assert hashlib.sha256(sentinel.read_bytes()).digest() == expected_hash, name

        assert not profile.exists()
        run('fresh-completion', finish, force=True)
        saved = json.loads(profile.read_text(encoding='utf-8'))
        assert saved['preferences']['setup.completed'] == 'true', 'Setup did not save completion'
        assert saved['preferences']['system.timezone']
        assert saved['preferences']['user.name']
        before = profile.read_bytes()
        run('read-only-completion', finish, force=True, read_only=True)
        assert profile.read_bytes() == before, 'Read-only setup changed the profile'
        host_autostart = None
        if os.name == 'nt':
            import winreg
            def autostart_value():
                try:
                    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Microsoft\Windows\CurrentVersion\Run') as key:
                        return winreg.QueryValueEx(key, 'OpenXMB')
                except FileNotFoundError:
                    return None
            host_autostart = autostart_value()
        run('reset-button', '0:ok;0:right;0:ok')
        if os.name == 'nt':
            assert autostart_value() == host_autostart, 'Reset changed host launch-at-login configuration'
        reset = json.loads(profile.read_text(encoding='utf-8'))
        assert reset['preferences']['setup.completed'] == 'false', 'Reset did not clear setup completion'
        run('post-reset-completion', finish, force=True)
        assert json.loads(profile.read_text())['preferences']['setup.completed'] == 'true'
        destination = root / 'new-user' / 'config.json'
        run('missing-profile-switch', f'0:profile={destination};' + finish.replace('0:', '0.05:'))
        assert json.loads(destination.read_text())['preferences']['setup.completed'] == 'true', 'Missing destination profile did not enter setup'
        print('PASS native setup: fresh save, read-only retry, actual shell reset, restart, missing-profile switching and unrelated media preservation')


if __name__ == '__main__':
    main()
