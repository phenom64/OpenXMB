#!/usr/bin/env python3
"""Read-only, bounded Windows focus observations; never performs activation/input.

capture_snapshot(shell_pid, game_pid, game_hwnd) returns JSON-safe data. Required
process/window identity failures raise SnapshotError; optional observations carry
explicit unavailable/error records. No titles, paths, text, or token contents.
"""
import argparse
import ctypes
from ctypes import wintypes as w
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import sys
import time


class SnapshotError(RuntimeError):
    """A supplied target cannot be safely identified for this snapshot."""


class GUIThreadInfo(ctypes.Structure):
    _fields_ = [('cbSize', w.DWORD), ('flags', w.DWORD),
                ('hwndActive', w.HWND), ('hwndFocus', w.HWND),
                ('hwndCapture', w.HWND), ('hwndMenuOwner', w.HWND),
                ('hwndMoveSize', w.HWND), ('hwndCaret', w.HWND), ('rcCaret', w.RECT)]


class LastInputInfo(ctypes.Structure):
    _fields_ = [('cbSize', w.UINT), ('dwTime', w.DWORD)]


class SidAndAttributes(ctypes.Structure):
    _fields_ = [('sid', ctypes.c_void_p), ('attributes', w.DWORD)]


def parse_hwnd(text):
    """Accept canonical positive decimal or lowercase 0x-prefixed hex only."""
    if not re.fullmatch(r'(?:[1-9][0-9]*|0x[1-9a-fA-F][0-9a-fA-F]*)', text):
        raise ValueError('window handle must be canonical positive decimal or 0x hexadecimal')
    result = int(text, 16 if text.startswith('0x') else 10)
    if result >= 1 << (ctypes.sizeof(ctypes.c_void_p) * 8):
        raise ValueError('window handle exceeds this Python process pointer width')
    return result


def _hwnd(value):
    return hex(value or 0)


def _error(api, code=None, detail=None):
    code = ctypes.get_last_error() if code is None else code
    return {'available': False, 'api': api, 'winerror': code,
            'detail': detail or (ctypes.FormatError(code).strip() if code else
                                 'API reported failure without an extended error code')}


class _WinAPI:
    def __init__(self):
        if os.name != 'nt':
            raise SnapshotError('Windows focus diagnostics require Windows')
        self.user = ctypes.WinDLL('user32', use_last_error=True)
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        self.advapi = ctypes.WinDLL('advapi32', use_last_error=True)
        self.enum_proc = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        signatures = [
            (self.user, 'GetForegroundWindow', [], w.HWND),
            (self.user, 'GetWindowThreadProcessId', [w.HWND, ctypes.POINTER(w.DWORD)], w.DWORD),
            (self.user, 'IsWindow', [w.HWND], w.BOOL),
            (self.user, 'IsWindowVisible', [w.HWND], w.BOOL),
            (self.user, 'IsIconic', [w.HWND], w.BOOL),
            (self.user, 'GetClassNameW', [w.HWND, w.LPWSTR, ctypes.c_int], ctypes.c_int),
            (self.user, 'GetGUIThreadInfo', [w.DWORD, ctypes.POINTER(GUIThreadInfo)], w.BOOL),
            (self.user, 'GetLastInputInfo', [ctypes.POINTER(LastInputInfo)], w.BOOL),
            (self.user, 'GetAsyncKeyState', [ctypes.c_int], ctypes.c_short),
            (self.user, 'EnumWindows', [self.enum_proc, w.LPARAM], w.BOOL),
            (self.kernel, 'OpenProcess', [w.DWORD, w.BOOL, w.DWORD], w.HANDLE),
            (self.kernel, 'CloseHandle', [w.HANDLE], w.BOOL),
            (self.kernel, 'WaitForSingleObject', [w.HANDLE, w.DWORD], w.DWORD),
            (self.kernel, 'GetProcessTimes', [w.HANDLE] + [ctypes.POINTER(w.FILETIME)] * 4, w.BOOL),
            (self.kernel, 'GetTickCount64', [], ctypes.c_ulonglong),
            (self.advapi, 'OpenProcessToken', [w.HANDLE, w.DWORD, ctypes.POINTER(w.HANDLE)], w.BOOL),
            (self.advapi, 'GetTokenInformation', [w.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                                w.DWORD, ctypes.POINTER(w.DWORD)], w.BOOL),
            (self.advapi, 'GetSidSubAuthorityCount', [ctypes.c_void_p], ctypes.POINTER(w.BYTE)),
            (self.advapi, 'GetSidSubAuthority', [ctypes.c_void_p, w.DWORD], ctypes.POINTER(w.DWORD)),
            (self.advapi, 'IsValidSid', [ctypes.c_void_p], w.BOOL),
        ]
        for library, name, args, result in signatures:
            function = getattr(library, name)
            function.argtypes, function.restype = args, result
        self.window_long = (self.user.GetWindowLongPtrW if ctypes.sizeof(ctypes.c_void_p) == 8
                            else self.user.GetWindowLongW)
        self.window_long.argtypes = [w.HWND, ctypes.c_int]
        self.window_long.restype = ctypes.c_ssize_t
        self.dpi = getattr(self.user, 'GetDpiForWindow', None)
        if self.dpi:
            self.dpi.argtypes, self.dpi.restype = [w.HWND], w.UINT

    def owner(self, hwnd):
        pid = w.DWORD()
        ctypes.set_last_error(0)
        thread = self.user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if not thread:
            raise SnapshotError(f'window {_hwnd(hwnd)} is invalid/unavailable: '
                                f'{_error("GetWindowThreadProcessId")}')
        return {'hwnd': _hwnd(hwnd), 'pid': pid.value, 'thread_id': thread}

    def open_process(self, pid):
        ctypes.set_last_error(0)
        # Query identity and observe exit only; no process mutation rights.
        handle = self.kernel.OpenProcess(0x1000 | 0x100000, False, pid)
        if not handle:
            raise SnapshotError(f'process {pid} cannot be identified: {_error("OpenProcess")}')
        try:
            self.require_live(handle, pid)
            creation, exit_time, kernel, user = (w.FILETIME() for _ in range(4))
            ctypes.set_last_error(0)
            if not self.kernel.GetProcessTimes(handle, ctypes.byref(creation), ctypes.byref(exit_time),
                                               ctypes.byref(kernel), ctypes.byref(user)):
                raise SnapshotError(f'process {pid} creation unavailable: {_error("GetProcessTimes")}')
            ticks = (creation.dwHighDateTime << 32) | creation.dwLowDateTime
            return handle, {'pid': pid, 'creation_filetime_100ns': str(ticks)}
        except BaseException:
            self.kernel.CloseHandle(handle)
            raise

    def require_live(self, handle, pid):
        ctypes.set_last_error(0)
        status = self.kernel.WaitForSingleObject(handle, 0)
        if status == 0:
            raise SnapshotError(f'process {pid} is no longer alive')
        if status != 258:  # WAIT_TIMEOUT is the sole live result.
            raise SnapshotError(f'process {pid} liveness unavailable: '
                                f'{_error("WaitForSingleObject")}; wait_status={status}')

    def token(self, process):
        token = w.HANDLE()
        ctypes.set_last_error(0)
        if not self.advapi.OpenProcessToken(process, 8, ctypes.byref(token)):
            return _error('OpenProcessToken')
        try:
            result = {}
            for name, info_class in [('elevation_type', 18), ('elevated', 20)]:
                value, length = w.DWORD(), w.DWORD()
                ctypes.set_last_error(0)
                if self.advapi.GetTokenInformation(token, info_class, ctypes.byref(value),
                                                   ctypes.sizeof(value), ctypes.byref(length)):
                    result[name] = (bool(value.value) if name == 'elevated' else
                                    {'value': value.value, 'name': {1: 'default', 2: 'full', 3: 'limited'}.get(value.value, 'unknown')})
                else:
                    result[name] = _error('GetTokenInformation')
            length = w.DWORD()
            ctypes.set_last_error(0)
            first = self.advapi.GetTokenInformation(token, 25, None, 0, ctypes.byref(length))
            error = ctypes.get_last_error()
            if first or error != 122 or not ctypes.sizeof(SidAndAttributes) <= length.value <= 4096:
                result['integrity'] = _error('GetTokenInformation(TokenIntegrityLevel)', error,
                                             'Expected bounded ERROR_INSUFFICIENT_BUFFER size query')
                return result
            buffer = ctypes.create_string_buffer(length.value)
            ctypes.set_last_error(0)
            if not self.advapi.GetTokenInformation(token, 25, buffer, len(buffer), ctypes.byref(length)):
                result['integrity'] = _error('GetTokenInformation(TokenIntegrityLevel)')
                return result
            sid = ctypes.cast(buffer, ctypes.POINTER(SidAndAttributes)).contents.sid
            # The SID returned by this query must remain inside the owned buffer.
            start, end = ctypes.addressof(buffer), ctypes.addressof(buffer) + len(buffer)
            if not sid or not start <= sid <= end - 8 or not self.advapi.IsValidSid(sid):
                result['integrity'] = _error('TokenIntegrityLevel', 0, 'Invalid SID in returned buffer')
                return result
            count = self.advapi.GetSidSubAuthorityCount(sid).contents.value
            if not count or sid + 8 + 4 * count > end:
                result['integrity'] = _error('TokenIntegrityLevel', 0, 'SID subauthorities exceed returned buffer')
                return result
            rid = self.advapi.GetSidSubAuthority(sid, count - 1).contents.value
            level = ('untrusted' if rid < 0x1000 else 'low' if rid < 0x2000 else
                     'medium' if rid < 0x2100 else 'medium_plus' if rid < 0x3000 else
                     'high' if rid < 0x4000 else 'system' if rid < 0x5000 else 'protected')
            result['integrity'] = {'rid': rid, 'level': level}
            return result
        finally:
            self.kernel.CloseHandle(token)

    def gui_thread(self, thread):
        info = GUIThreadInfo()
        info.cbSize = ctypes.sizeof(info)
        ctypes.set_last_error(0)
        if not self.user.GetGUIThreadInfo(thread, ctypes.byref(info)):
            return _error('GetGUIThreadInfo')
        bits = {'caret_blinking': 1, 'in_move_size': 2, 'in_menu_mode': 4,
                'system_menu_mode': 8, 'popup_menu_mode': 16}
        result = {'available': True, 'flags': info.flags,
                  'decoded_flags': {name: bool(info.flags & bit) for name, bit in bits.items()},
                  'unknown_flag_bits': info.flags & ~31}
        # Omit caret rectangles/content. HWNDs are identities only, not inspected.
        for name in ('hwndActive', 'hwndFocus', 'hwndCapture', 'hwndMenuOwner', 'hwndMoveSize'):
            result[name] = _hwnd(getattr(info, name))
        return result

    def window(self, hwnd, required_pid=None):
        before = self.owner(hwnd)
        if required_pid is not None and before['pid'] != required_pid:
            raise SnapshotError(f'window {_hwnd(hwnd)} belongs to PID {before["pid"]}, expected {required_pid}')
        result = dict(before)
        result['visible'] = bool(self.user.IsWindowVisible(hwnd))
        result['iconic'] = bool(self.user.IsIconic(hwnd))
        name = ctypes.create_unicode_buffer(256)
        ctypes.set_last_error(0)
        count = self.user.GetClassNameW(hwnd, name, len(name))
        result['class'] = name.value if count else _error('GetClassNameW')
        for key, index in [('style', -16), ('exstyle', -20)]:
            ctypes.set_last_error(0)
            value = self.window_long(hwnd, index)
            error = ctypes.get_last_error()
            result[key] = _error('GetWindowLongPtrW/GetWindowLongW', error) if not value and error else hex(value & 0xffffffff)
        if self.dpi:
            value = self.dpi(hwnd)
            result['dpi'] = value if value else _error('GetDpiForWindow', 0, 'API returned zero; extended errors are not documented')
        else:
            result['dpi'] = _error('GetDpiForWindow', 0, 'API unavailable on this Windows version')
        result['gui_thread'] = self.gui_thread(before['thread_id'])
        after = self.owner(hwnd)
        if before != after or not self.user.IsWindow(hwnd):
            raise SnapshotError(f'window {_hwnd(hwnd)} changed ownership or disappeared during observation')
        result['ownership_stable'] = True
        return result

    def shell_windows(self, pid):
        handles = []
        truncated = False

        @self.enum_proc
        def visit(hwnd, _):
            nonlocal truncated
            owner = w.DWORD()
            if self.user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner)) and owner.value == pid:
                if len(handles) < 16:
                    handles.append(hwnd)
                else:
                    truncated = True
            return True

        ctypes.set_last_error(0)
        if not self.user.EnumWindows(visit, 0):
            return _error('EnumWindows')
        windows = []
        for hwnd in handles:
            try:
                windows.append(self.window(hwnd, pid))
            except SnapshotError as error:
                windows.append({'hwnd': _hwnd(hwnd), 'available': False, 'detail': str(error)})
        return {'windows': windows, 'truncated': truncated, 'limit': 16,
                'scope': 'top-level windows owned by supplied shell PID only'}


def capture_snapshot(shell_pid: int, game_pid: int, game_hwnd: int) -> dict:
    """Capture one observation, rejecting invalid/dead/changed required identities.

    Process handles pin identity during collection. This cannot detect an HWND
    destroyed and reused on the SAME process/thread between two WinAPI reads.
    """
    for name, pid in [('shell_pid', shell_pid), ('game_pid', game_pid)]:
        if type(pid) is not int or not 0 < pid <= 0xffffffff:
            raise SnapshotError(f'{name} must be a positive DWORD PID')
    if type(game_hwnd) is not int or not 0 < game_hwnd < 1 << (8 * ctypes.sizeof(ctypes.c_void_p)):
        raise SnapshotError('game_hwnd must be a nonzero window handle within pointer width')
    api = _WinAPI()
    started_ns = time.monotonic_ns()
    result = {'schema_version': 1, 'wall_time_utc': datetime.now(timezone.utc).isoformat(),
              'monotonic_ns': started_ns, 'collector_pid': os.getpid(),
              'pointer_bits': 8 * ctypes.sizeof(ctypes.c_void_p),
              'target': {'shell_pid': shell_pid, 'game_pid': game_pid, 'game_hwnd': _hwnd(game_hwnd)},
              'processes': {}}
    handles = {}

    def process(pid):
        if pid not in handles:
            handle, identity = api.open_process(pid)
            handles[pid] = (handle, identity)
        handle, identity = handles[pid]
        return {**identity, 'security': api.token(handle)}

    try:
        result['processes']['shell'] = process(shell_pid)
        result['processes']['game'] = process(game_pid)
        target_before = api.owner(game_hwnd)
        if target_before['pid'] != game_pid:
            raise SnapshotError(f'window {_hwnd(game_hwnd)} belongs to PID {target_before["pid"]}, expected {game_pid}')
        result['game_window'] = api.window(game_hwnd, game_pid)
        foreground = api.user.GetForegroundWindow()
        if foreground:
            try:
                result['foreground_window'] = api.window(foreground)
                foreground_pid = result['foreground_window']['pid']
                try:
                    result['processes']['foreground'] = process(foreground_pid)
                except SnapshotError as error:
                    result['processes']['foreground'] = {'pid': foreground_pid, 'available': False, 'detail': str(error)}
            except SnapshotError as error:
                result['foreground_window'] = {'hwnd': _hwnd(foreground), 'available': False, 'detail': str(error)}
        else:
            result['foreground_window'] = {'hwnd': '0x0', 'available': False,
                                           'detail': 'GetForegroundWindow returned NULL (transition/no foreground)'}
        result['shell_windows'] = api.shell_windows(shell_pid)
        info = LastInputInfo()
        info.cbSize = ctypes.sizeof(info)
        ctypes.set_last_error(0)
        if api.user.GetLastInputInfo(ctypes.byref(info)):
            age = ((api.kernel.GetTickCount64() & 0xffffffff) - info.dwTime) & 0xffffffff
            result['last_input'] = {'available': True, 'tick_32': info.dwTime, 'age_ms_modulo_2_32': age,
                                    'scope': 'current logon session; tick may be non-monotonic'}
        else:
            result['last_input'] = _error('GetLastInputInfo')
        keys = {'shift': 0x10, 'control': 0x11, 'alt': 0x12, 'left_windows': 0x5b, 'right_windows': 0x5c}
        result['modifiers'] = {'down': {name: bool(api.user.GetAsyncKeyState(vk) & 0x8000)
                                       for name, vk in keys.items()},
                               'caveat': 'False may mean up or inaccessible desktop/UIPI; no key events recorded'}
        if api.owner(game_hwnd) != target_before:
            raise SnapshotError(f'game window {_hwnd(game_hwnd)} changed ownership during snapshot')
        # Re-open PIDs and compare creation identities; never substitute a reused PID.
        for pid, (handle, identity) in handles.items():
            api.require_live(handle, pid)
            check_handle, after = api.open_process(pid)
            try:
                if after != identity:
                    raise SnapshotError(f'process {pid} identity changed during snapshot')
            finally:
                api.kernel.CloseHandle(check_handle)
        end_foreground = api.user.GetForegroundWindow()
        result['foreground_at_end'] = _hwnd(end_foreground)
        result['foreground_unchanged'] = foreground == end_foreground
        result['completed_monotonic_ns'] = time.monotonic_ns()
        result['duration_ms'] = (result['completed_monotonic_ns'] - started_ns) / 1_000_000
        result['limitations'] = ['Sequential observation, not an atomic snapshot',
                                'Same-thread HWND reuse between reads cannot be ruled out',
                                'Does not prove foreground admission, input queue attachment, or physical Guide routing']
        return result
    finally:
        for handle, _ in handles.values():
            api.kernel.CloseHandle(handle)


def write_snapshot(output, snapshot):
    """Exclusive-create UTF-8 JSON; never overwrite and never create directories."""
    serialized = json.dumps(snapshot, indent=2, ensure_ascii=True, allow_nan=False) + '\n'
    with Path(output).open('x', encoding='utf-8', newline='\n') as stream:
        stream.write(serialized)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shell-pid', type=int, required=True)
    parser.add_argument('--game-pid', type=int, required=True)
    parser.add_argument('--game-hwnd', type=parse_hwnd, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        # Capture/validate before opening a file: invalid identities leave no output.
        snapshot = capture_snapshot(args.shell_pid, args.game_pid, args.game_hwnd)
        write_snapshot(args.output, snapshot)
    except (SnapshotError, OSError, ValueError) as error:
        print(f'focus diagnostics: {error}', file=sys.stderr)
        return 1
    print(str(args.output))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
