#!/usr/bin/env python3
"""Check real startup playback and Enter dismissal using SDL's dummy audio sink.

The native mixer still decodes and clocks the complete cue. This verifies its
lifetime, not physical speaker output or a connected controller.
"""
import argparse
import ctypes
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


def run_case(app, assets, output, skip):
    directory = output / ("enter-dismiss" if skip else "natural-startup")
    directory.mkdir(parents=True, exist_ok=False)
    config = json.loads((assets.parent / "config.json").read_text(encoding="utf-8-sig"))
    config.setdefault("render", {}).update({"max-fps": 30, "sample-count": 1, "vsync": False})
    configuration = directory / "config.json"
    configuration.write_text(json.dumps(config), encoding="utf-8")
    env = dict(os.environ)
    for key in ("OPENXMB_FIXED_BOOT_SECONDS", "OPENXMB_FIXED_WAVE_SECONDS", "AUROREENGINE_HEADLESS_TERMINAL"):
        env.pop(key, None)
    env.update({"SPDLOG_LEVEL": "debug", "SDL_AUDIODRIVER": "dummy",
                "AUROREENGINE_HEADLESS": "1", "AUROREENGINE_HEADLESS_AUDIO": "1",
                "AUROREENGINE_HEADLESS_FRAMES": "20000", "AUROREENGINE_HEADLESS_WIDTH": "960",
                "AUROREENGINE_HEADLESS_HEIGHT": "540", "OPENXMB_HEADLESS_STARTUP": "1",
                "AUROREENGINE_HEADLESS_OUTPUT_DIR": str(directory),
                "AUROREENGINE_HEADLESS_OUTPUT_PATTERN": "latest.png",
                "OPENXMB_CONFIG": str(configuration), "OPENXMB_CONFIG_READ_ONLY": "1",
                "OPENXMB_DATA_DIR": str(directory / "data"), "XMB_ASSET_DIR": str(assets)})
    # The engine implements stdin polling only on Linux. Exercise the real SDL
    # window with Windows key messages when a pipe cannot inject commands.
    native = os.name == "nt"
    if native:
        env["AUROREENGINE_HEADLESS"] = "0"
    log = directory / "application.log"
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen([str(app), "--width", "960", "--height", "540", "--no-fullscreen"],
                                   cwd=app.parent, env=env, stdin=subprocess.PIPE,
                                   stdout=stream, stderr=subprocess.STDOUT, text=True,
                                   creationflags=subprocess.CREATE_NO_WINDOW if native else 0)
        started = time.monotonic()
        def wait_until(seconds):
            while time.monotonic() - started < seconds:
                if process.poll() is not None:
                    raise RuntimeError(f"Startup exited early: {log}")
                time.sleep(0.05)
        def send(command):
            if native:
                from ctypes import wintypes
                user32 = ctypes.windll.user32
                callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
                windows = []
                def find_window(handle, _):
                    pid = wintypes.DWORD()
                    user32.GetWindowThreadProcessId(handle, ctypes.byref(pid))
                    if pid.value == process.pid and user32.IsWindowVisible(handle):
                        windows.append(handle)
                    return True
                user32.EnumWindows(callback_type(find_window), 0)
                if not windows:
                    raise RuntimeError(f"No native SDL window for {process.pid}")
                user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
                if command == "quit":
                    user32.PostMessageW(windows[0], 0x0010, 0, 0)  # WM_CLOSE
                else:
                    up = " up " in command
                    flags = 1 | (0x1C << 16) | ((3 << 30) if up else 0)
                    user32.PostMessageW(windows[0], 0x0101 if up else 0x0100, 0x0D, flags)
                return
            process.stdin.write(command + "\n")
            process.stdin.flush()
        try:
            if skip:
                wait_until(2.5)
                send("keyboard down 40 13 0")  # Identity cannot be skipped.
                wait_until(2.7)
                send("keyboard up 40 13 0")
                wait_until(9.0)
                if not native:
                    shutil.copyfile(directory / "latest.png", directory / "warning.png")
                send("keyboard down 40 13 0")
                wait_until(10.5)
                send("keyboard down 40 13 0")  # Held repeat must stay consumed.
                wait_until(10.7)
                send("keyboard up 40 13 0")
                wait_until(11.0)
            else:
                wait_until(18.5)
            send("quit")
            process.wait(timeout=12)
            if process.returncode:
                raise RuntimeError(f"Startup failed ({process.returncode}): {log}")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    events = {}
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.search(r"\[([\d-]+ [\d:.]+)\].*?(Startup (?:cue started|cue finished|warning dismissed|UI reveal complete))", line)
        if match:
            events.setdefault(match[2], []).append(datetime.fromisoformat(match[1]))
    def once(name):
        if len(events.get(name, [])) != 1:
            raise AssertionError(f"Expected one {name}: {log}")
        return events[name][0]
    cue_start = once("Startup cue started")
    cue_duration = (once("Startup cue finished") - cue_start).total_seconds()
    if not 6.4 <= cue_duration <= 7.2:
        raise AssertionError(f"Startup cue was shortened/delayed: {cue_duration}s ({log})")
    complete = once("Startup UI reveal complete")
    if skip:
        dismissed = once("Startup warning dismissed")
        reveal = (complete - dismissed).total_seconds()
        if not 0.95 <= reveal <= 1.2:
            raise AssertionError(f"Dismissal did not retain the one-second reveal: {reveal}s")
        # The cue starts after its decoder loads; the visual clock starts in
        # preload. Exact warning boundaries are covered by the timeline tests.
        if (dismissed - cue_start).total_seconds() < 7.0:
            raise AssertionError("The identity intro was skipped before the warning")
    elif events.get("Startup warning dismissed") or (complete - cue_start).total_seconds() < 16.5:
        raise AssertionError("Natural startup did not retain its complete timeline")
    return {"case": directory.name, "cue_seconds": cue_duration, "log": str(log)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    with ThreadPoolExecutor(max_workers=2) as pool:
        results = list(pool.map(lambda skip: run_case(args.app.resolve(), args.assets.resolve(),
                                                    args.output.resolve(), skip), [False, True]))
    report = json.dumps(results, indent=2)
    (args.output / "report.json").write_text(report + "\n", encoding="utf-8")
    print(report)
