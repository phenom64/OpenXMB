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
    parser.add_argument('--child', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if os.name != 'nt':
        raise SystemExit('This fixture requires Windows.')
    from PIL import Image, ImageGrab
    app, child, root = args.app.resolve(), args.child.resolve(), args.output.resolve()
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
            if entry.exe.lower() in {'xms.bin.exe', 'openxmb.exe', app.name.lower()}:
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
    user.SetForegroundWindow.argtypes = [w.HWND]
    user.IsWindowVisible.argtypes = [w.HWND]
    user.IsIconic.argtypes = [w.HWND]
    user.SetWindowLongPtrW.argtypes = [w.HWND, ctypes.c_int, ctypes.c_ssize_t]
    user.SetWindowPos.argtypes = [w.HWND, w.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, w.UINT]
    user.GetWindowRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
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
        raise AssertionError(message)

    def key(hwnd, vk):
        scan = user.MapVirtualKeyW(vk, 0)
        bits = (scan << 16) | 1 | ((1 << 24) if vk in (0x25, 0x26, 0x27, 0x28) else 0)
        user.PostMessageW(hwnd, 0x0100, vk, bits)
        user.PostMessageW(hwnd, 0x0101, vk, bits | (3 << 30))
        time.sleep(.18)

    profile = root / 'data/profiles/default'
    profile.mkdir(parents=True)
    (root / 'media/Pictures').mkdir(parents=True)
    ready = root / 'fixture-ready'
    (root / 'data/profiles.json').write_text(json.dumps({'version': 1, 'active_profile': 'default',
        'profiles': [{'id': 'default', 'name': 'Overlay Fixture'}]}))
    config = root / 'config.json'
    config.write_text(json.dumps({'shell': {}, 'preferences': {'setup.completed': 'true',
        'startup.intro': 'Off', 'startup.warning': 'Off', 'audio.startup': 'Off', 'games.launch': 'Minimise'}}))
    (profile / 'library.json').write_text(json.dumps({'version': 1, 'games': [{'id': 'overlay-fixture',
        'title': 'Overlay Fixture', 'executable': str(child), 'working_directory': str(root),
        'arguments': [str(ready), '60000']}]}))
    env = {k: v for k, v in os.environ.items() if not k.startswith(('OPENXMB_SETUP_', 'OPENXMB_HARNESS_',
        'OPENXMB_FIXED_', 'OPENXMB_INITIAL_', 'AUROREENGINE_HEADLESS'))}
    env.update({'OPENXMB_CONFIG': str(config), 'OPENXMB_DATA_DIR': str(root / 'data'),
        'XMB_ASSET_DIR': str(app.parent / 'shell'), 'XMB_LOCALE_DIR': str(app.parent / 'locales'),
        'OPENXMB_FORCE_SETUP': '0', 'OPENXMB_INITIAL_CATEGORY': 'game', 'OPENXMB_HARNESS_ACTIONS': '1:ok',
        'SDL_AUDIODRIVER': 'dummy', 'SPDLOG_LEVEL': 'info'})
    shell_hwnd = game_hwnd = None
    with (root / 'native.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen([str(app), '--no-fullscreen', '--width', '960', '--height', '540',
            '--force-dir', str(root / 'media')], cwd=app.parent, env=env, stdout=log, stderr=log,
            creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            shell_hwnd = wait(lambda: next(iter(windows(process.pid)), None), 'Shell window missing')
            wait(ready.exists, 'Owned game did not launch')
            game_hwnd = int(ready.read_text())
            wait(lambda: user.IsIconic(shell_hwnd), 'Shell did not minimize')
            monitor = Monitor(); monitor.size = ctypes.sizeof(monitor)
            assert user.GetMonitorInfoW(user.MonitorFromWindow(game_hwnd, 2), ctypes.byref(monitor))
            bounds = monitor.bounds
            box = (bounds.left, bounds.top, bounds.right, bounds.bottom)
            user.SetWindowLongPtrW(game_hwnd, -16, 0x90000000)  # visible popup, fixture only
            assert user.SetWindowPos(game_hwnd, None, bounds.left, bounds.top,
                bounds.right - bounds.left, bounds.bottom - bounds.top, 0x0020 | 0x0040)
            user.SetForegroundWindow(game_hwnd)
            wait(lambda: user.GetForegroundWindow() == game_hwnd, 'Fixture focus unavailable')
            time.sleep(.3)
            baseline = ImageGrab.grab(box)
            baseline.save(root / 'game.png')

            def open_overlay():
                user.PostMessageW(shell_hwnd, 0x0312, 0x4f58, 0)
                return wait(lambda: next((hwnd for hwnd in windows(process.pid, 'OpenXMB.LayeredOverlay.v1')
                    if user.IsWindowVisible(hwnd) and user.GetForegroundWindow() == hwnd), None),
                    'Transparent XMB did not appear and take focus', seconds=8)

            overlay = open_overlay()
            assert user.IsIconic(shell_hwnd), 'Ordinary shell restored underneath transparent host'
            time.sleep(.5)
            rendered = ImageGrab.grab(box)
            rendered.save(root / 'overlay-game.png')
            # Far from menus: both halves of the real game remain visible through black dimming.
            for nx in (.1, .9):
                point = (int(rendered.width * nx), int(rendered.height * .8))
                before, after = baseline.getpixel(point)[:3], rendered.getpixel(point)[:3]
                assert all(abs(a - b * .5) <= 12 for a, b in zip(after, before)), (point, before, after)
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
                for nx in (.1, .9):
                    point = (int(captured.width * nx), int(captured.height * .8))
                    assert captured.getpixel(point)[:3] == baseline.getpixel(point)[:3], 'Screenshot included overlay dimming'

            overlay = open_overlay()
            user.PostMessageW(game_hwnd, 0x0010, 0, 0)
            wait(lambda: not user.IsIconic(shell_hwnd), 'Game exit did not restore normal shell')
            wait(lambda: not user.IsWindowVisible(overlay), 'Overlay survived game exit')
            print('PASS transparent pixels, Game/Photo navigation, Back focus, clean screenshot, exit lifecycle')
        finally:
            if game_hwnd:
                user.PostMessageW(game_hwnd, 0x0010, 0, 0)
            if shell_hwnd:
                user.PostMessageW(shell_hwnd, 0x0010, 0, 0)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.terminate(); process.wait(timeout=5)
        assert process.returncode == 0, (root / 'native.log').read_text(encoding='utf-8')


if __name__ == '__main__':
    main()
