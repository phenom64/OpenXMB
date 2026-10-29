#!/usr/bin/env python3
"""Opt-in real Windows fixture for minimized-shell session exit and return."""
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time


def main():
    if os.name != 'nt':
        raise SystemExit('This controlled desktop fixture currently requires Windows.')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--child', type=Path, required=True)
    args = parser.parse_args()
    app, child = args.app.resolve(), args.child.resolve()
    user = ctypes.windll.user32
    enum_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [enum_proc, wintypes.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    for name in ('IsWindowVisible', 'IsIconic'):
        getattr(user, name).argtypes = [wintypes.HWND]
    user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]

    def window(pid):
        found = []
        @enum_proc
        def visit(hwnd, _):
            owner = wintypes.DWORD()
            user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == pid and user.IsWindowVisible(hwnd):
                found.append(hwnd)
            return True
        user.EnumWindows(visit, 0)
        return found[0] if found else None

    def wait(predicate, message, limit=20):
        deadline = time.monotonic() + limit
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(.01)
        raise AssertionError(message)

    for duration in (900, 1):
        with tempfile.TemporaryDirectory(prefix='openxmb-native-session-') as temporary:
            root = Path(temporary)
            profile = root / 'data/profiles/default'
            profile.mkdir(parents=True)
            ready = root / 'fixture-ready'
            (root / 'data/profiles.json').write_text(json.dumps({
                'version': 1, 'active_profile': 'default',
                'profiles': [{'id': 'default', 'name': 'Fixture'}],
            }), encoding='utf-8')
            config = root / 'config.json'
            config.write_text(json.dumps({'shell': {}, 'preferences': {
                'setup.completed': 'true', 'startup.intro': 'Off',
                'startup.warning': 'Off', 'audio.startup': 'Off', 'games.launch': 'Minimise',
            }}), encoding='utf-8')
            (profile / 'library.json').write_text(json.dumps({'version': 1, 'games': [{
                'id': 'window-fixture', 'title': 'Window Fixture', 'executable': str(child),
                'working_directory': str(root), 'arguments': [str(ready), str(duration)],
            }]}), encoding='utf-8')
            env = {key: value for key, value in os.environ.items() if not key.startswith((
                'OPENXMB_SETUP_', 'OPENXMB_HARNESS_', 'OPENXMB_FIXED_', 'OPENXMB_INITIAL_', 'AUROREENGINE_HEADLESS'))}
            env.update({'OPENXMB_CONFIG': str(config), 'OPENXMB_DATA_DIR': str(root / 'data'),
                        'XMB_ASSET_DIR': str(app.parent / 'shell'), 'XMB_LOCALE_DIR': str(app.parent / 'locales'),
                        'OPENXMB_FORCE_SETUP': '0', 'OPENXMB_INITIAL_CATEGORY': 'game',
                        'OPENXMB_HARNESS_ACTIONS': '2:ok', 'SDL_AUDIODRIVER': 'dummy', 'SPDLOG_LEVEL': 'warn'})
            with (root / 'native.log').open('w', encoding='utf-8') as log:
                process = subprocess.Popen([str(app), '--no-fullscreen', '--width', '960', '--height', '540'],
                                           cwd=app.parent, env=env, stdout=log, stderr=log,
                                           creationflags=subprocess.CREATE_NO_WINDOW)
                hwnd = None
                try:
                    hwnd = wait(lambda: window(process.pid), 'OpenXMB window did not appear')
                    wait(ready.exists, 'Game fixture did not launch')
                    if duration > 100:
                        wait(lambda: user.IsIconic(hwnd), 'OpenXMB did not minimize after launch')
                    journal = profile / 'sessions/history.json'
                    def exited():
                        try:
                            records = json.loads(journal.read_text(encoding='utf-8'))['recent']
                            return records and records[-1]['state'] == 1
                        except (FileNotFoundError, json.JSONDecodeError):
                            return False
                    try:
                        wait(exited, 'Session exit was not observed while OpenXMB was minimized')
                    except AssertionError:
                        print('Session journal:', journal.read_text(encoding='utf-8') if journal.exists() else 'missing', flush=True)
                        print('Shell log:', (root / 'native.log').read_text(encoding='utf-8'), flush=True)
                        raise
                    wait(lambda: not user.IsIconic(hwnd), 'OpenXMB did not restore after session exit')
                    assert process.poll() is None, 'OpenXMB exited with the fixture'
                finally:
                    if hwnd:
                        user.PostMessageW(hwnd, 0x0010, 0, 0)
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.terminate()
                        process.wait(timeout=5)
                assert process.returncode == 0, (root / 'native.log').read_text(encoding='utf-8')
            print(f'PASS native session exit/return: {duration} ms fixture')


if __name__ == '__main__':
    main()
