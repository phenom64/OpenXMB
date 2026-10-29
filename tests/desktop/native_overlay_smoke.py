#!/usr/bin/env python3
"""Opt-in Windows compositor/input/focus/capture test over an owned fixture."""
import argparse
import ctypes
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--child', type=Path)
    parser.add_argument('--game-executable', type=Path, help='Opt in to an installed game instead of the controlled fixture')
    parser.add_argument('--steam-executable', type=Path)
    parser.add_argument('--steam-app-id')
    parser.add_argument('--title')
    parser.add_argument('--capture-only', action='store_true', help='Skip the initial Back/reopen cycle while diagnosing capture')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--allow-other-previews', action='store_true', help='Explicitly allow independent previews; restore prior focus afterward')
    args = parser.parse_args()
    if os.name != 'nt':
        raise SystemExit('This fixture requires Windows.')
    from PIL import Image, ImageGrab
    actual_game = args.game_executable is not None
    if actual_game and (not args.steam_executable or not args.steam_app_id):
        parser.error('Installed game tests require --steam-executable and --steam-app-id')
    if not actual_game and not args.child:
        parser.error('--child is required for the controlled fixture')
    app, root = args.app.resolve(), args.output.resolve()
    child = args.game_executable.resolve() if actual_game else args.child.resolve()
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    class ProcessEntry(ctypes.Structure):
        _fields_ = [('size', w.DWORD), ('usage', w.DWORD), ('pid', w.DWORD),
            ('heap', ctypes.c_size_t), ('module', w.DWORD), ('threads', w.DWORD),
            ('parent', w.DWORD), ('priority', w.LONG), ('flags', w.DWORD), ('exe', w.WCHAR * 260)]
    kernel.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = w.HANDLE
    kernel.Process32FirstW.argtypes = [w.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel.Process32NextW.argtypes = [w.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel.CloseHandle.argtypes = [w.HANDLE]
    inventory = kernel.CreateToolhelp32Snapshot(2, 0)
    if inventory == w.HANDLE(-1).value:
        raise SystemExit('Cannot verify that other previews are closed.')
    try:
        entry = ProcessEntry(); entry.size = ctypes.sizeof(entry)
        available = kernel.Process32FirstW(inventory, ctypes.byref(entry))
        while available:
            if actual_game and entry.exe.lower() == child.name.lower():
                raise SystemExit('The selected game is already running; it will not be adopted or closed.')
            if not args.allow_other_previews and entry.exe.lower() in {'xms.bin.exe', 'openxmb.exe', app.name.lower()}:
                raise SystemExit('Close other OpenXMB previews before this desktop focus fixture.')
            available = kernel.Process32NextW(inventory, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(inventory)
    root.mkdir(parents=True, exist_ok=False)
    user = ctypes.WinDLL('user32', use_last_error=True)
    visit_type = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    user.EnumWindows.argtypes = [visit_type, w.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
    user.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
    user.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
    user.GetForegroundWindow.restype = w.HWND
    user.SetProcessDpiAwarenessContext.argtypes = [w.HANDLE]
    user.SetProcessDpiAwarenessContext(w.HANDLE(-4))
    prior_focus = user.GetForegroundWindow()
    class KeyboardInput(ctypes.Structure):
        _fields_ = [('vk', w.WORD), ('scan', w.WORD), ('flags', w.DWORD), ('time', w.DWORD), ('extra', ctypes.c_size_t)]
    class MouseInput(ctypes.Structure):
        _fields_ = [('x', w.LONG), ('y', w.LONG), ('data', w.DWORD), ('flags', w.DWORD), ('time', w.DWORD), ('extra', ctypes.c_size_t)]
    class InputData(ctypes.Union):
        _fields_ = [('key', KeyboardInput), ('mouse', MouseInput)]
    class Input(ctypes.Structure):
        _anonymous_ = ('data',)
        _fields_ = [('kind', w.DWORD), ('data', InputData)]
    user.SendInput.argtypes = [w.UINT, ctypes.POINTER(Input), ctypes.c_int]
    user.SendInput.restype = w.UINT
    def shortcut():
        events = (Input * 6)()
        for event, vk, flags in zip(events, (0x11, 0x10, 0x73, 0x73, 0x10, 0x11), (0, 0, 0, 2, 2, 2)):
            event.kind = 1; event.key.vk = vk; event.key.flags = flags
        assert user.SendInput(6, events, ctypes.sizeof(Input)) == 6, 'Configured shortcut could not be sent'

    user.SetForegroundWindow.argtypes = [w.HWND]
    user.IsWindowVisible.argtypes = [w.HWND]
    user.IsIconic.argtypes = [w.HWND]
    user.SetWindowLongPtrW.argtypes = [w.HWND, ctypes.c_int, ctypes.c_ssize_t]
    user.SetWindowPos.argtypes = [w.HWND, w.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, w.UINT]
    user.GetWindowRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
    user.GetClientRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
    user.ClientToScreen.argtypes = [w.HWND, ctypes.POINTER(w.POINT)]
    user.MonitorFromWindow.argtypes = [w.HWND, w.DWORD]
    user.MonitorFromWindow.restype = w.HANDLE
    class Monitor(ctypes.Structure):
        _fields_ = [('size', w.DWORD), ('bounds', w.RECT), ('work', w.RECT), ('flags', w.DWORD)]
    user.GetMonitorInfoW.argtypes = [w.HANDLE, ctypes.POINTER(Monitor)]

    def windows(pid, class_name=None):
        result = []
        @visit_type
        def visit(hwnd, _):
            owner = w.DWORD()
            user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            name = ctypes.create_unicode_buffer(256)
            user.GetClassNameW(hwnd, name, len(name))
            if owner.value == pid and (class_name is None or name.value == class_name):
                result.append(hwnd)
            return True
        user.EnumWindows(visit, 0)
        return result

    def wait(predicate, message, seconds=20):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            result = predicate()
            if result:
                return result
            if process.poll() is not None:
                raise AssertionError(f'{message}: shell exited {process.returncode}')
            time.sleep(.02)
        foreground = user.GetForegroundWindow()
        owner = w.DWORD()
        if foreground:
            user.GetWindowThreadProcessId(foreground, ctypes.byref(owner))
        ImageGrab.grab().save(root / 'failure-desktop.png')
        print('Focus diagnostic:', {'foreground': foreground, 'owner': owner.value,
            'shell': shell_hwnd, 'game': game_hwnd,
            'overlay_windows': [(hwnd, bool(user.IsWindowVisible(hwnd)))
                for hwnd in windows(process.pid, 'OpenXMB.LayeredOverlay.v1')],
            'shortcut_key_states': {vk: user.GetAsyncKeyState(vk) for vk in (0x11, 0x10, 0x73)}}, flush=True)
        raise AssertionError(message)

    def key(hwnd, vk):
        scan = user.MapVirtualKeyW(vk, 0)
        bits = (scan << 16) | 1 | ((1 << 24) if vk in (0x25, 0x26, 0x27, 0x28) else 0)
        user.PostMessageW(hwnd, 0x0100, vk, bits)
        user.PostMessageW(hwnd, 0x0101, vk, bits | (3 << 30))
        time.sleep(.18)

    profile = root / 'data/profiles/default'
    profile.mkdir(parents=True)
    for category in ('Pictures', 'Music', 'Videos'):
        (root / 'media' / category).mkdir(parents=True)
    ready = root / 'fixture-ready'
    (root / 'data/profiles.json').write_text(json.dumps({'version': 1, 'active_profile': 'default',
        'profiles': [{'id': 'default', 'name': 'Overlay Fixture'}]}))
    config = root / 'config.json'
    config.write_text(json.dumps({'shell': {}, 'preferences': {'setup.completed': 'true',
        'startup.intro': 'Off', 'startup.warning': 'Off', 'audio.startup': 'Off', 'games.launch': 'Minimise', 'session.hotkey': 'Ctrl+Shift+F4'}}))
    game = {'id': 'overlay-fixture', 'title': 'Overlay Fixture', 'executable': str(child),
        'working_directory': str(root), 'arguments': [str(ready), '60000']}
    if actual_game:
        game = {'id': 'steam-integration', 'title': args.title or child.stem, 'runner': 'steam',
            'platform': 'Steam', 'content': args.steam_app_id, 'title_id': args.steam_app_id,
            'executable': str(args.steam_executable.resolve()), 'working_directory': str(child.parent),
            'launch_override': {'mode': 'provider', 'executable': str(child), 'arguments': [], 'working_directory': ''}}
    (profile / 'library.json').write_text(json.dumps({'version': 1, 'games': [game]}))
    journal = profile / 'sessions/history.json'
    def game_record():
        try:
            records = json.loads(journal.read_text(encoding='utf-8'))['recent']
            return next((item for item in reversed(records) if item['identity'] == game['id']), {})
        except (FileNotFoundError, json.JSONDecodeError): return {}
    env = {k: v for k, v in os.environ.items() if not k.startswith(('OPENXMB_SETUP_', 'OPENXMB_HARNESS_',
        'OPENXMB_FIXED_', 'OPENXMB_INITIAL_', 'AUROREENGINE_HEADLESS'))}
    env.update({'OPENXMB_CONFIG': str(config), 'OPENXMB_DATA_DIR': str(root / 'data'),
        'XMB_ASSET_DIR': str(app.parent / 'shell'), 'XMB_LOCALE_DIR': str(app.parent / 'locales'),
        'OPENXMB_FORCE_SETUP': '0', 'OPENXMB_INITIAL_CATEGORY': 'game', 'OPENXMB_HARNESS_ACTIONS': '1:ok',
        'SDL_AUDIODRIVER': 'dummy', 'SPDLOG_LEVEL': 'debug'})
    shell_hwnd = game_hwnd = None
    with (root / 'native.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen([str(app), '--no-fullscreen', '--width', '960', '--height', '540',
            '--force-dir', str(root / 'media')], cwd=app.parent, env=env, stdout=log, stderr=log,
            creationflags=subprocess.CREATE_NO_WINDOW)

        def open_overlay():
            shortcut()
            return wait(lambda: next((hwnd for hwnd in windows(process.pid, 'OpenXMB.LayeredOverlay.v1')
                if user.IsWindowVisible(hwnd) and user.GetForegroundWindow() == hwnd), None),
                'Transparent XMB did not appear and take focus', seconds=8)

        def focus_game():
            user.SetForegroundWindow(game_hwnd)
            if user.GetForegroundWindow() != game_hwnd:
                # A registered shortcut is genuine activation intent. Return
                # through the XMB instead of trying to defeat foreground policy.
                bootstrap = open_overlay()
                key(bootstrap, 0x1b)
            wait(lambda: user.GetForegroundWindow() == game_hwnd, 'XMB could not return game focus')

        try:
            shell_hwnd = wait(lambda: next((hwnd for hwnd in windows(process.pid) if user.IsWindowVisible(hwnd)), None), 'Visible shell window missing')
            if actual_game:
                def associated_window():
                    record = game_record()
                    if record.get('tracking') == 3 and record.get('process_id', -1) > 0:
                        return next((hwnd for hwnd in windows(record['process_id']) if user.IsWindowVisible(hwnd)), None)
                game_hwnd = wait(associated_window, 'Steam game was not associated with a new window', seconds=50)
                focus_game()
                time.sleep(18)  # Finish startup without sending gameplay input.
            else:
                wait(ready.exists, 'Owned game did not launch')
                game_hwnd = int(ready.read_text())
            wait(lambda: user.IsIconic(shell_hwnd), 'Shell did not minimize')
            monitor = Monitor(); monitor.size = ctypes.sizeof(monitor)
            assert user.GetMonitorInfoW(user.MonitorFromWindow(game_hwnd, 2), ctypes.byref(monitor))
            bounds = monitor.bounds
            box = (bounds.left, bounds.top, bounds.right, bounds.bottom)
            if actual_game:
                client = w.RECT(); origin = w.POINT()
                assert user.GetClientRect(game_hwnd, ctypes.byref(client))
                assert user.ClientToScreen(game_hwnd, ctypes.byref(origin))
                box = (origin.x, origin.y, origin.x + client.right, origin.y + client.bottom)
            else:
                user.SetWindowLongPtrW(game_hwnd, -16, 0x90000000)  # fixture only
                assert user.SetWindowPos(game_hwnd, None, bounds.left, bounds.top,
                    bounds.right - bounds.left, bounds.bottom - bounds.top, 0x0020 | 0x0040)
            focus_game()
            time.sleep(.3)
            baseline = ImageGrab.grab(box)
            baseline.save(root / 'game.png')

            overlay = open_overlay()
            assert user.IsIconic(shell_hwnd), 'Ordinary shell restored underneath transparent host'
            time.sleep(.5)
            rendered = ImageGrab.grab(box)
            rendered.save(root / 'overlay-game.png')
            # Far from menus: both halves of the real game remain visible through black dimming.
            for nx in (() if actual_game else (.1, .9)):
                point = (int(rendered.width * nx), int(rendered.height * .8))
                before, after = baseline.getpixel(point)[:3], rendered.getpixel(point)[:3]
                assert all(abs(a - b * .35) <= 12 for a, b in zip(after, before)), (point, before, after)
            if not args.capture_only:
                key(overlay, 0x1b)  # root Back returns to game
                wait(lambda: not user.IsWindowVisible(overlay), 'Back did not hide overlay')
                wait(lambda: user.GetForegroundWindow() == game_hwnd, 'Back did not restore game focus')
                overlay = open_overlay()
            for _ in range(4):
                key(overlay, 0x25)  # Game -> Photo
            ImageGrab.grab(box).save(root / 'overlay-photo.png')
            key(overlay, 0x0d)  # Save Screenshot in Photo
            shots = root / 'media/Pictures/Screenshots'
            screenshot = wait(lambda: next(iter(shots.rglob('*.png')), None), 'Photo screenshot was not saved')
            wait(lambda: user.GetForegroundWindow() == game_hwnd, 'Screenshot did not restore game focus')
            with Image.open(screenshot) as captured:
                assert captured.size == baseline.size, (captured.size, baseline.size)
                assert captured.convert('RGB').getbbox(), 'Screenshot is entirely black'
                for nx in (() if actual_game else (.1, .9)):
                    point = (int(captured.width * nx), int(captured.height * .8))
                    assert captured.getpixel(point)[:3] == baseline.getpixel(point)[:3], 'Screenshot included overlay dimming'

            overlay = open_overlay()
            if actual_game:
                key(overlay, 0x0d)  # Quit Game confirmation in the real XMB
                time.sleep(.3)
                key(overlay, 0x27)
                key(overlay, 0x0d)
                wait(lambda: game_record().get('state') == 1, 'Game did not honor graceful Quit Game', seconds=25)
                record = game_record()
                assert record['tracking'] == 3 and record['duration_ms'] > 0, record
                print('PASS real Steam launch, pinned observation, overlay, screenshot and graceful Quit Game:', game['title'])
            else:
                user.PostMessageW(game_hwnd, 0x0010, 0, 0)
            wait(lambda: not user.IsIconic(shell_hwnd), 'Game exit did not restore normal shell')
            wait(lambda: not user.IsWindowVisible(overlay), 'Overlay survived game exit')
            print('PASS transparent pixels, Game/Photo navigation, Back focus, clean screenshot, exit lifecycle')
        finally:
            foreground = user.GetForegroundWindow()
            owner = w.DWORD()
            if foreground: user.GetWindowThreadProcessId(foreground, ctypes.byref(owner))
            restore_focus = owner.value == process.pid or foreground == game_hwnd
            if game_hwnd:
                user.PostMessageW(game_hwnd, 0x0010, 0, 0)
            if shell_hwnd:
                user.PostMessageW(shell_hwnd, 0x0010, 0, 0)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.terminate(); process.wait(timeout=5)
            if restore_focus and prior_focus: user.SetForegroundWindow(prior_focus)
        assert process.returncode == 0, (root / 'native.log').read_text(encoding='utf-8')


if __name__ == '__main__':
    main()
