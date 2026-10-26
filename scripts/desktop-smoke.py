#!/usr/bin/env python3
"""Exercise the real native shell with generated, isolated desktop fixtures.

Run after building XMS, for example:
  . /workspace/.openxmb-env/activate.sh
  python3 scripts/desktop-smoke.py --app build/default/XMS.bin

Screenshots, fixture inputs, logs, configuration and report.json are retained in
one fresh run directory. Required tools: Python/Pillow, ffmpeg, ffprobe. Tesseract
strengthens visible-text assertions when available. No browser page load or
physical optical drive is claimed by this renderer/UI smoke.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.util
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess
import sys
import time
import wave
import zipfile

try:
    from PIL import Image, ImageChops, ImageDraw, ImageStat
except ImportError as error:
    raise SystemExit("desktop-smoke requires Pillow: install the Python pillow package") from error

ROOT = Path(__file__).resolve().parent.parent
MAGENTA = (185, 27, 115)
CYAN = (24, 232, 232)
PHOTO = [(232, 35, 61), (30, 212, 105), (33, 84, 225), (245, 205, 35)]
PDF_FIRST = (160, 42, 210)
PAGE_SECOND = (245, 140, 35)
COMIC_FIRST = (15, 190, 205)
COVER = (230, 30, 150)


class SmokeFailure(RuntimeError):
    def __init__(self, message: str, exit_code: int = 1):
        super().__init__(message)
        self.exit_code = exit_code if exit_code > 0 else 128 - exit_code


def checked(command: list[str], *, cwd: Path | None = None, timeout: float = 45) -> str:
    result = subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=timeout)
    if result.returncode:
        raise SmokeFailure(f"Command failed ({result.returncode}): {command!r}\n{result.stdout[-6000:]}", result.returncode)
    return result.stdout


def json_write(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def solid(path: Path, color: tuple[int, int, int], label: str = "", size=(640, 480)) -> None:
    image = Image.new("RGB", size, color)
    if label:
        draw = ImageDraw.Draw(image)
        draw.text((24, 24), label, fill="white", stroke_width=1)
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)


@dataclass
class Fixtures:
    directory: Path
    photo: Path
    audio: Path
    video: Path
    pdf: Path
    comic: Path
    theme: Path


def fixtures(directory: Path) -> Fixtures:
    directory.mkdir(parents=True, exist_ok=True)
    photo = directory / "photos/fixture-photo.png"
    photo.parent.mkdir(parents=True)
    image = Image.new("RGB", (640, 480))
    draw = ImageDraw.Draw(image)
    for box, color in zip([(0, 0, 320, 240), (320, 0, 640, 240),
                           (0, 240, 320, 480), (320, 240, 640, 480)], PHOTO):
        draw.rectangle(box, fill=color)
    image.save(photo)
    solid(photo.parent / "second-photo.png", PAGE_SECOND, "SECOND PHOTO")
    audio = directory / "music/smoke-audio.wav"
    audio.parent.mkdir(parents=True)
    # A real stereo PCM stream, with valid RIFF INFO tags for metadata probing.
    rate, seconds = 48000, 30
    block = bytearray()
    for index in range(rate):
        value = int(math.sin(index * math.tau * 440 / rate) * 9000)
        block += struct.pack("<hh", value, value)
    with wave.open(str(audio), "wb") as stream:
        stream.setnchannels(2)
        stream.setsampwidth(2)
        stream.setframerate(rate)
        for _ in range(seconds):
            stream.writeframesraw(block)
    tags = bytearray(b"INFO")
    for tag, value in [(b"INAM", b"SMOKEAUDIO\0"), (b"IART", b"Native Smoke\0"),
                       (b"IPRD", b"Generated Fixtures\0")]:
        tags += tag + struct.pack("<I", len(value)) + value
        if len(value) & 1:
            tags += b"\0"
    data = bytearray(audio.read_bytes())
    data += b"LIST" + struct.pack("<I", len(tags)) + tags
    struct.pack_into("<I", data, 4, len(data) - 8)
    audio.write_bytes(data)
    solid(audio.parent / "cover.png", COVER, "SMOKEAUDIO", (256, 256))
    audio_tags = json.loads(checked(["ffprobe", "-v", "error", "-show_entries", "format_tags", "-of", "json", str(audio)]))
    if audio_tags.get("format", {}).get("tags", {}).get("title") != "SMOKEAUDIO":
        raise SmokeFailure("The generated audio fixture is missing its expected title metadata")
    json_write(audio.parent / "fixture-metadata.json", audio_tags)
    video = directory / "video/smoke-video.mkv"
    video.parent.mkdir(parents=True)
    subtitle = video.with_suffix(".srt")
    subtitle.write_text("1\n00:00:00,000 --> 00:00:39,500\nSMOKESUBTITLE\n", encoding="utf-8")
    checked(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
             "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=12",
             "-f", "lavfi", "-i", "sine=frequency=660:sample_rate=48000",
             "-i", str(subtitle), "-t", "40", "-map", "0:v", "-map", "1:a", "-map", "2:s",
             "-c:v", "mpeg4", "-q:v", "3", "-c:a", "pcm_s16le", "-c:s", "srt",
             "-metadata", "title=SMOKEVIDEO", "-disposition:s:0", "default", str(video)])
    streams = json.loads(checked(["ffprobe", "-v", "error", "-show_streams", "-of", "json", str(video)]))["streams"]
    kinds = [stream["codec_type"] for stream in streams]
    if not all(kind in kinds for kind in ("video", "audio", "subtitle")):
        raise SmokeFailure("The generated video fixture is missing video, audio or subtitles")
    json_write(video.parent / "fixture-streams.json", streams)
    pdf = directory / "documents/smoke-document.pdf"
    pdf.parent.mkdir(parents=True)
    first, second = Image.new("RGB", (640, 480), PDF_FIRST), Image.new("RGB", (640, 480), PAGE_SECOND)
    ImageDraw.Draw(first).text((40, 35), "SMOKEPDFONE", fill="white", stroke_width=1)
    ImageDraw.Draw(second).text((40, 35), "SMOKEPDFTWO", fill="white", stroke_width=1)
    first.save(pdf, "PDF", save_all=True, append_images=[second], resolution=72)
    comic = directory / "documents/smoke-comic.cbz"
    with zipfile.ZipFile(comic, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, color in (("01.png", COMIC_FIRST), ("02.png", PAGE_SECOND)):
            page = Image.new("RGB", (640, 480), color)
            ImageDraw.Draw(page).ellipse((220, 160, 420, 360), outline="white", width=8)
            buffer = io.BytesIO()
            page.save(buffer, "PNG")
            archive.writestr(name, buffer.getvalue())
    theme = directory / "smoke-native-theme.zip"
    background, icon = io.BytesIO(), io.BytesIO()
    Image.new("RGB", (640, 480), MAGENTA).save(background, "PNG")
    icon_image = Image.new("RGBA", (96, 96), (0, 0, 0, 0))
    ImageDraw.Draw(icon_image).rounded_rectangle((5, 5, 91, 91), radius=14, fill=(*CYAN, 255))
    icon_image.save(icon, "PNG")
    manifest = {
        "format": "openxmb-theme", "version": 1, "name": "Smoke Native Theme",
        "background": "background.png", "preview": "background.png",
        "settings": {"iconColor": "#ffffff", "textColor": "#ffffff"},
        "icons": {name: "icon.png" for name in (
            "icon_user", "icon_setting", "icon_photo", "icon_music", "icon_video",
            "icon_game", "icon_network", "icon_friend", "icon_category_users.png",
            "icon_category_settings.png", "icon_category_game.png", "xmb_icon_000.png")}}
    with zipfile.ZipFile(theme, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("theme.json", json.dumps(manifest))
        archive.writestr("background.png", background.getvalue())
        archive.writestr("icon.png", icon.getvalue())
    return Fixtures(directory, photo, audio, video, pdf, comic, theme)


def crop(image: Image.Image, box=(0, 0, 1, 1)) -> Image.Image:
    width, height = image.size
    return image.crop(tuple(int(value * (width if index % 2 == 0 else height)) for index, value in enumerate(box)))


def pixels(image):
    return image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata()


def color_ratio(image: Image.Image, color: tuple[int, int, int], tolerance=40, box=(0, 0, 1, 1)) -> float:
    values = crop(image, box).convert("RGB").resize((160, 90))
    count = sum(max(abs(channel - expected) for channel, expected in zip(pixel, color)) <= tolerance
                for pixel in pixels(values))
    return count / (160 * 90)


def foreground(image: Image.Image, box=(0.05, 0.05, 0.95, 0.9)) -> float:
    values = crop(image, box).convert("RGB").resize((320, 180))
    return sum(min(pixel) > 170 and max(pixel) - min(pixel) < 80 for pixel in pixels(values)) / (320 * 180)


def difference(first: Image.Image, second: Image.Image, box=(0.05, 0.1, 0.95, 0.8)) -> float:
    a, b = crop(first, box).convert("RGB").resize((320, 180)), crop(second, box).convert("RGB").resize((320, 180))
    return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / (3 * 255)


# SDL physical scancodes and symbols, matching the engine's real keyboard route.
KEYS = {"enter": (40, 13), "escape": (41, 27), "backspace": (42, 8), "tab": (43, 9),
        "space": (44, 32), "left": (80, 1073741904), "right": (79, 1073741903),
        "up": (82, 1073741906), "down": (81, 1073741905), "shift": (225, 1073742049)}


class NativeInput:
    """X11 input to this run's SDL window; used only with --native-window."""
    def __init__(self):
        self.x11 = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
        self.xtest = ctypes.CDLL(ctypes.util.find_library("Xtst") or "libXtst.so.6")
        pointer, window = ctypes.c_void_p, ctypes.c_ulong
        self.x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x11.XOpenDisplay.restype = pointer
        self.x11.XDefaultRootWindow.argtypes = [pointer]
        self.x11.XDefaultRootWindow.restype = window
        self.x11.XInternAtom.argtypes = [pointer, ctypes.c_char_p, ctypes.c_int]
        self.x11.XInternAtom.restype = window
        self.x11.XQueryTree.argtypes = [pointer, window, ctypes.POINTER(window), ctypes.POINTER(window), ctypes.POINTER(ctypes.POINTER(window)), ctypes.POINTER(ctypes.c_uint)]
        self.x11.XGetWindowProperty.argtypes = [pointer, window, window, ctypes.c_long, ctypes.c_long, ctypes.c_int, window, ctypes.POINTER(window), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(window), ctypes.POINTER(window), ctypes.POINTER(pointer)]
        self.x11.XFree.argtypes = [pointer]
        self.x11.XSetInputFocus.argtypes = [pointer, window, ctypes.c_int, window]
        self.x11.XRaiseWindow.argtypes = [pointer, window]
        self.x11.XKeysymToKeycode.argtypes = [pointer, window]
        self.x11.XKeysymToKeycode.restype = ctypes.c_uint
        self.x11.XSync.argtypes = [pointer, ctypes.c_int]
        self.xtest.XTestFakeKeyEvent.argtypes = [pointer, ctypes.c_uint, ctypes.c_int, window]
        self.display = self.x11.XOpenDisplay(None)
        if not self.display:
            raise SmokeFailure("Native-window smoke needs an X11 display", 2)
        self.root = self.x11.XDefaultRootWindow(self.display)
        self.pid_atom = self.x11.XInternAtom(self.display, b"_NET_WM_PID", 0)

    def locate(self, process_id, parent=None, depth=0):
        if depth > 5:
            return 0
        parent = self.root if parent is None else parent
        actual, count, remaining, data = ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_void_p()
        format_ = ctypes.c_int()
        self.x11.XGetWindowProperty(self.display, parent, self.pid_atom, 0, 1, 0, 6,
            ctypes.byref(actual), ctypes.byref(format_), ctypes.byref(count), ctypes.byref(remaining), ctypes.byref(data))
        owns = bool(data and format_.value == 32 and count.value == 1 and ctypes.cast(data, ctypes.POINTER(ctypes.c_ulong))[0] == process_id)
        if data:
            self.x11.XFree(data)
        if owns:
            return parent
        root, actual_parent, children, number = ctypes.c_ulong(), ctypes.c_ulong(), ctypes.POINTER(ctypes.c_ulong)(), ctypes.c_uint()
        if not self.x11.XQueryTree(self.display, parent, ctypes.byref(root), ctypes.byref(actual_parent), ctypes.byref(children), ctypes.byref(number)):
            return 0
        found = 0
        for index in range(min(number.value, 1024)):
            found = self.locate(process_id, children[index], depth + 1)
            if found:
                break
        if children:
            self.x11.XFree(ctypes.cast(children, ctypes.c_void_p))
        return found

    def line(self, process_id, command):
        fields = command.split()
        if len(fields) != 5 or fields[0] != "keyboard":
            raise SmokeFailure("Invalid native keyboard command")
        window = self.locate(process_id)
        if not window:
            raise SmokeFailure("This run's native SDL window was not found")
        self.x11.XRaiseWindow(self.display, window)
        self.x11.XSetInputFocus(self.display, window, 2, 0)
        scan, symbol = int(fields[2]), int(fields[3])
        special = {40: 0xff0d, 41: 0xff1b, 42: 0xff08, 43: 0xff09,
                   79: 0xff53, 80: 0xff51, 81: 0xff54, 82: 0xff52, 225: 0xffe1}
        keycode = self.x11.XKeysymToKeycode(self.display, special.get(scan, symbol))
        if not keycode:
            raise SmokeFailure(f"No X11 keycode for SDL scancode {scan}")
        self.xtest.XTestFakeKeyEvent(self.display, keycode, int(fields[1] == "down"), 0)
        self.x11.XSync(self.display, 0)

    def close_window(self, process_id):
        window = self.locate(process_id)
        if not window:
            return
        class Data(ctypes.Union):
            _fields_ = [("bytes", ctypes.c_char * 20), ("shorts", ctypes.c_short * 10), ("longs", ctypes.c_long * 5)]
        class ClientMessage(ctypes.Structure):
            _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                        ("display", ctypes.c_void_p), ("window", ctypes.c_ulong), ("message_type", ctypes.c_ulong),
                        ("format", ctypes.c_int), ("data", Data)]
        class Event(ctypes.Union):
            _fields_ = [("client", ClientMessage), ("padding", ctypes.c_long * 24)]
        event = Event()
        event.client.type, event.client.window = 33, window
        event.client.display, event.client.format = self.display, 32
        event.client.message_type = self.x11.XInternAtom(self.display, b"WM_PROTOCOLS", 0)
        event.client.data.longs[0] = self.x11.XInternAtom(self.display, b"WM_DELETE_WINDOW", 0)
        self.x11.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.POINTER(Event)]
        self.x11.XSendEvent(self.display, window, 0, 0, ctypes.byref(event))
        self.x11.XSync(self.display, 0)


class Session:
    def __init__(self, runner: "Runner", name: str, arguments: list[str], *, reuse: Path | None = None):
        self.runner, self.name = runner, name
        self.directory = runner.output / name
        self.directory.mkdir(parents=True, exist_ok=False)
        self.state_root = reuse if reuse else self.directory
        self.configuration = self.state_root / "config.json"
        if reuse is None:
            config = json.loads((ROOT / "config.json").read_text())
            config.setdefault("_meta", {})["config_version"] = 5
            config.setdefault("render", {}).update({"sample-count": 1, "max-fps": 30, "vsync": False, "show-fps": False, "show-mem": False, "icon-glass-refraction": False})
            config.setdefault("shell", {}).update({"background-type": "original", "font-path": "default",
                "pictures-path": str(runner.fixtures.photo.parent), "music-path": str(runner.fixtures.audio.parent),
                "videos-path": str(runner.fixtures.video.parent)})
            json_write(self.configuration, config)
        self.frames = self.directory / "frames"
        self.frames.mkdir()
        self.log = self.directory / "application.log"
        environment = dict(os.environ)
        environment.update({"OPENXMB_CONFIG": str(self.configuration),
            "OPENXMB_DATA_DIR": str(self.state_root / "data/OpenXMB"),
            "XDG_DATA_HOME": str(self.state_root / "data"), "XDG_CONFIG_HOME": str(self.state_root / "config"),
            "XDG_CACHE_HOME": str(self.state_root / "cache"), "SDL_AUDIODRIVER": "dummy", "SDL_VIDEODRIVER": "dummy",
            "DREAMRENDER_HEADLESS": "1", "DREAMRENDER_HEADLESS_AUDIO": "1", "DREAMRENDER_HEADLESS_FRAMES": "20000",
            "DREAMRENDER_HEADLESS_WIDTH": str(runner.args.width), "DREAMRENDER_HEADLESS_HEIGHT": str(runner.args.height),
            "DREAMRENDER_HEADLESS_OUTPUT_DIR": str(self.frames), "DREAMRENDER_HEADLESS_OUTPUT_PATTERN": "frame-{:06d}.png",
            "OPENXMB_FIXED_UNIX_SECONDS": "1781524800", "OPENXMB_FIXED_WAVE_SECONDS": "4.0",
            "OPENXMB_FIXED_BOOT_SECONDS": "17.0", "OPENXMB_HEADLESS_STARTUP": "0"})
        environment.pop("DREAMRENDER_HEADLESS_TERMINAL", None)
        for key in ("OPENXMB_INITIAL_SETTINGS_MENU", "OPENXMB_INITIAL_SETTINGS_SELECTION", "OPENXMB_OPEN_INITIAL_SETTINGS_CHOICE"):
            environment.pop(key, None)
        environment["OPENXMB_INITIAL_CATEGORY"] = "users"
        if runner.args.native_window:
            environment["DREAMRENDER_HEADLESS"] = "0"
            environment.pop("SDL_VIDEODRIVER", None)
        environment["XMB_ASSET_DIR"] = str(runner.assets)
        environment["XMB_LOCALE_DIR"] = str(runner.locales)
        runtime = self.state_root / "runtime"
        runtime.mkdir(exist_ok=True)
        runtime.chmod(0o700)
        environment["XDG_RUNTIME_DIR"] = str(runtime)
        for directory in ("data/OpenXMB", "config", "cache"):
            (self.state_root / directory).mkdir(parents=True, exist_ok=True)
        self.log_handle = self.log.open("w", encoding="utf-8")
        self.process = subprocess.Popen([str(runner.app), "--width", str(runner.args.width), "--height", str(runner.args.height),
                                         "--no-fullscreen", *arguments], cwd=self.directory, env=environment,
                                        stdin=subprocess.PIPE, stdout=self.log_handle, stderr=subprocess.STDOUT,
                                        text=True, start_new_session=os.name != "nt")
        self.seen = -1
        self.native_counter = 0
        self.last_native_capture = 0.0
        self.latest_image: Image.Image | None = None
        self.started = time.monotonic()
        self.evidence: list[str] = []
        self.cleanup_terminated = False

    def __enter__(self):
        return self

    def __exit__(self, error_type, error, traceback):
        try:
            self.close(check=error is None)
        finally:
            self.runner.sessions.append({"name": self.name, "directory": str(self.directory),
                "application_exit": self.process.returncode, "terminated_for_cleanup": self.cleanup_terminated, "seconds": round(time.monotonic() - self.started, 3),
                "evidence": self.evidence})

    def close(self, *, check=True):
        if self.process.poll() is None:
            try:
                if self.runner.native:
                    self.runner.native.close_window(self.process.pid)
                else:
                    self.process.stdin.write("quit\n")
                    self.process.stdin.flush()
                self.process.wait(timeout=12)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                if self.process.poll() is None:
                    self.cleanup_terminated = True
                    if os.name == "nt":
                        self.process.terminate()
                    else:
                        os.killpg(self.process.pid, signal.SIGTERM)
                    try:
                        self.process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        self.process.kill()
                        self.process.wait()
        self.log_handle.close()
        if check and self.process.returncode:
            raise SmokeFailure(f"{self.name}: application exited {self.process.returncode}; see {self.log}", self.process.returncode)
        text = self.log.read_text(errors="replace")
        if check and not re.search(r"Configuration loaded|Welcome to OpenXMB", text):
            raise SmokeFailure(f"{self.name}: startup confirmation missing from {self.log}")
        if check and re.search(r"Validation Error|terminate called|Segmentation fault|Video playback failed|Failed to open video", text, re.I):
            raise SmokeFailure(f"{self.name}: application reported a runtime failure; see {self.log}")

    def frame(self) -> tuple[int, Image.Image] | None:
        if self.runner.native and time.monotonic() - self.last_native_capture > 0.1:
            self.native_counter += 1
            destination = self.frames / f"frame-{self.native_counter:06d}.png"
            checked(["import", "-silent", "-window", "root", "PNG24:" + str(destination)], timeout=10)
            self.last_native_capture = time.monotonic()
        files = sorted(self.frames.glob("frame-*.png"))
        for path in reversed(files[-3:]):
            number = int(path.stem.split("-")[-1])
            if number <= self.seen:
                continue
            try:
                image = Image.open(path).convert("RGB")
                image.load()
            except (OSError, ValueError, SyntaxError):
                continue
            self.seen = number
            self.latest_image = image
            # Retain bounded raw capture history as well as named evidence.
            for old in files[:-8]:
                try:
                    old.unlink()
                except FileNotFoundError:
                    pass
            return number, image
        return None

    def wait(self, predicate, evidence: str, *, timeout=None, delay=0.0) -> Image.Image:
        deadline = time.monotonic() + (timeout or self.runner.args.timeout)
        earliest = time.monotonic() + delay
        last_error = "no complete native frames yet"
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise SmokeFailure(f"{self.name}: exited before {evidence}; see {self.log}", self.process.returncode or 1)
            current = self.frame()
            if current and time.monotonic() >= earliest:
                number, image = current
                if image.size != (self.runner.args.width, self.runner.args.height):
                    raise SmokeFailure(f"{self.name}: native output size is {image.size}")
                try:
                    okay = predicate(image)
                except (OSError, ValueError) as error:
                    okay, last_error = False, str(error)
                if okay:
                    destination = self.directory / f"{evidence}.png"
                    image.save(destination)
                    self.evidence.append(str(destination))
                    return image
                last_error = f"frame {number} did not meet the expected fixture/foreground check"
            time.sleep(0.075)
        if self.latest_image is not None:
            self.latest_image.save(self.directory / "failure-last-frame.png")
        raise SmokeFailure(f"{self.name}: timeout waiting for {evidence}: {last_error}; see {self.directory}")

    def send_lines(self, lines):
        if self.runner.native:
            # XTest preserves event order in SDL's queue; it does not have the
            # C++ stdin buffering issue of the offscreen command adapter.
            for line in lines:
                if self.process.poll() is not None:
                    raise SmokeFailure(f"{self.name}: application exited during native keyboard input", self.process.returncode or 1)
                self.runner.native.line(self.process.pid, line)
            time.sleep(0.05)
            self.frame()
            return
        # The engine polls the OS descriptor before getline. Feed one complete
        # command between frames so C++ stream buffering cannot hide later keys.
        for line in lines:
            names = sorted(self.frames.glob("frame-*.png"))
            barrier = int(names[-1].stem.split("-")[-1]) if names else -1
            if self.runner.native:
                self.runner.native.line(self.process.pid, line)
            else:
                self.process.stdin.write(line)
                self.process.stdin.flush()
            deadline = time.monotonic() + min(15, self.runner.args.timeout)
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    raise SmokeFailure(f"{self.name}: application exited during native keyboard input", self.process.returncode or 1)
                current = self.frame()
                if current and current[0] >= barrier + 2:
                    break
                time.sleep(0.01)
            else:
                raise SmokeFailure(f"{self.name}: native renderer stopped producing frames during keyboard input")

    def key(self, name: str, count=1):
        if name in KEYS:
            scan, symbol = KEYS[name]
        elif len(name) == 1 and "a" <= name <= "z":
            scan, symbol = ord(name) - ord("a") + 4, ord(name)
        else:
            raise SmokeFailure(f"Unsupported smoke key: {name}")
        self.send_lines(line for _ in range(count) for line in
                        (f"keyboard down {scan} {symbol} 0\n", f"keyboard up {scan} {symbol} 0\n"))

    def type(self, text: str):
        punctuation = {"-": (45, 45), "=": (46, 61), "[": (47, 91), "]": (48, 93),
                       "\\": (49, 92), ";": (51, 59), "'": (52, 39), "`": (53, 96),
                       ",": (54, 44), ".": (55, 46), "/": (56, 47), " ": (44, 32)}
        payload = []
        for character in text:
            upper = character.isalpha() and character.isupper()
            if upper:
                payload.append("keyboard down 225 1073742049 0\n")
            lower = character.lower()
            if "a" <= lower <= "z":
                scan, symbol = ord(lower) - ord("a") + 4, ord(lower)
            elif character.isdigit() and ord(character) < 128:
                scan, symbol = (39 if character == "0" else 30 + int(character) - 1), ord(character)
            elif character in punctuation:
                scan, symbol = punctuation[character]
            else:
                raise SmokeFailure(f"Fixture path has an unsupported physical keyboard character: {character!r}")
            payload.extend([f"keyboard down {scan} {symbol} 0\n", f"keyboard up {scan} {symbol} 0\n"])
            if upper:
                payload.append("keyboard up 225 1073742049 0\n")
        self.send_lines(payload)

    def wait_path(self, path: Path, *, present=True):
        deadline = time.monotonic() + self.runner.args.timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise SmokeFailure(f"{self.name}: application exited while waiting for {path}", self.process.returncode or 1)
            if path.exists() == present:
                return
            self.frame()
            time.sleep(0.075)
        raise SmokeFailure(f"{self.name}: expected file state was not reached: {path} (present={present})")

    def wait_file(self, path: Path, predicate=lambda value: True, timeout=None):
        deadline = time.monotonic() + (timeout or self.runner.args.timeout)
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise SmokeFailure(f"{self.name}: application exited while waiting for {path}", self.process.returncode or 1)
            if path.is_file():
                try:
                    value = json.loads(path.read_text())
                    if predicate(value):
                        return value
                except (json.JSONDecodeError, OSError):
                    pass
            self.frame()
            time.sleep(0.075)
        raise SmokeFailure(f"{self.name}: expected persistent state was not written: {path}")


class Runner:
    def __init__(self, args):
        self.args = args
        self.app = args.app.resolve()
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S")
        self.output = (args.output or ROOT / "build/default/desktop-smoke" / f"{stamp}-{os.getpid()}").resolve()
        self.output.mkdir(parents=True, exist_ok=False)
        self.sessions: list[dict] = []
        self.results: list[dict] = []
        self.ocr = shutil.which("tesseract") if not args.no_ocr else None
        self.native = NativeInput() if args.native_window and not args.generate_only else None
        if args.require_ocr and not self.ocr:
            raise SmokeFailure("Tesseract is required for this run", 2)
        candidates = [args.assets] if args.assets else [self.app.parent / "share/shell", self.app.parent.parent / "share/shell"]
        self.assets = next((path.resolve() for path in candidates if path and path.is_dir()), None)
        self.locales = self.app.parent / "locales"
        if not self.locales.is_dir():
            self.locales = self.app.parent.parent / "share/locale"
        self.default_hash = hashlib.sha256((ROOT / "config.json").read_bytes()).hexdigest()
        self.fixtures = fixtures(self.output / "fixtures")

    def text(self, image, box=(0, 0, 1, 1), *, line=False):
        if not self.ocr:
            return ""
        sample = crop(image, box).convert("RGB")
        sample = sample.resize((sample.width * 3, sample.height * 3))
        buffer = io.BytesIO()
        sample.save(buffer, "PNG")
        result = subprocess.run([self.ocr, "stdin", "stdout", "--psm", "7" if line else "11"],
                                input=buffer.getvalue(), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=15,
                                env={**os.environ, "OMP_THREAD_LIMIT": "1"})
        if result.returncode:
            raise SmokeFailure("Tesseract failed", result.returncode)
        return result.stdout.decode(errors="replace")

    def panel_ready(self, image, title):
        if foreground(image) < 0.001:
            return False
        if not self.ocr:
            return color_ratio(image, (6, 9, 14), 20, (0.1, 0.2, 0.9, 0.8)) > 0.25
        normalized = re.sub(r"[^a-z0-9]", "", self.text(image, (0.07, 0.07, 0.95, 0.27)).lower())
        return re.sub(r"[^a-z0-9]", "", title.lower()) in normalized

    def root(self):
        with Session(self, "root", []) as session:
            session.wait(lambda image: foreground(image, (0.12, 0.18, 0.9, 0.45)) > 0.002 and
                         (not self.ocr or "localprofiles" in re.sub(r"[^a-z]", "", self.text(image, (0.1, 0.15, 0.9, 0.75)).lower())), "root-foreground")

    def photo(self):
        with Session(self, "photo", ["--open-media", str(self.fixtures.photo)]) as session:
            session.wait(lambda image: all(color_ratio(image, color) > 0.055 for color in PHOTO), "decoded-photo-quadrants")
            session.key("right")
            session.wait(lambda image: color_ratio(image, PAGE_SECOND) > 0.25, "next-photo")

    def music(self):
        with Session(self, "music", ["--open-media", str(self.fixtures.audio)]) as session:
            session.wait(lambda image: color_ratio(image, COVER, 35) > 0.015 and foreground(image, (0.07, 0.07, 0.75, 0.23)) > 0.004 and
                         (not self.ocr or re.search(r"SMOKEAUDI[O0]", re.sub(r"\s", "", self.text(image, (0.07, 0.07, 0.85, 0.23))).upper())),
                         "music-artwork-and-metadata")
            if self.ocr:
                session.wait(lambda image: "playing" in self.text(image, (0.07, 0.58, 0.36, 0.65), line=True).lower(), "music-playing")
            session.key("space")
            paused = session.wait(lambda image: not self.ocr or "paused" in self.text(image, (0.07, 0.58, 0.36, 0.65), line=True).lower(),
                                  "music-paused", delay=0.15)
            frozen = session.wait(lambda image: difference(paused, image, (0.08, 0.825, 0.92, 0.845)) < 0.0005, "music-paused-progress", delay=0.4)
            session.key("space")
            session.wait(lambda image: difference(frozen, image, (0.08, 0.825, 0.92, 0.845)) > 0.0007,
                         "music-resumed-progress", delay=0.6)
            session.key("i")
            session.wait(lambda image: foreground(image, (0.42, 0.27, 0.94, 0.70)) > 0.006, "music-information")

    def video(self):
        with Session(self, "video", ["--open-media", str(self.fixtures.video)]) as session:
            first = session.wait(lambda image: color_ratio(image, (255, 0, 0), 65) > 0.035 and color_ratio(image, (0, 255, 0), 65) > 0.035,
                                 "video-decoded-colors")
            session.wait(lambda image: difference(first, image, (0.1, 0.15, 0.9, 0.7)) > 0.005,
                         "video-advancing-frames", delay=0.3)
            session.key("enter")
            paused = session.wait(lambda image: color_ratio(image, (255, 0, 0), 65) > 0.03, "video-paused", delay=0.2)
            session.wait(lambda image: difference(paused, image, (0.1, 0.15, 0.9, 0.7)) < 0.0007,
                         "video-paused-frame-stable", delay=0.4)
            session.key("enter")
            session.wait(lambda image: difference(paused, image, (0.1, 0.15, 0.9, 0.7)) > 0.005,
                         "video-resumed-frames", delay=0.4)
            session.key("s")
            session.wait(lambda image: ("subtitles" in self.text(image, (0.59, 0.04, 0.95, 0.28)).lower()) if self.ocr else
                         color_ratio(image, (6, 9, 15), 35, (0.58, 0.05, 0.96, 0.8)) > 0.50, "video-subtitle-menu")
            session.key("down")
            session.key("enter")
            session.wait(lambda image: ("SMOKESUBTITLE" in re.sub(r"\s", "", self.text(image, (0.1, 0.73, 0.9, 0.86))).upper()) if self.ocr
                         else foreground(image, (0.2, 0.75, 0.8, 0.84)) > 0.004,
                         "video-visible-subtitle", delay=0.3)

    def document(self, name, source, color):
        with Session(self, name, ["--open-media", str(source)]) as session:
            session.wait(lambda image: color_ratio(image, color, 45) > 0.20, "decoded-page-one")
            session.key("right")
            session.wait(lambda image: color_ratio(image, PAGE_SECOND, 45) > 0.20, "decoded-page-two")

    def panels(self):
        for panel, title in (("browser", "Internet Browser"), ("emulation", "Emulation"),
                             ("discs", "Optical Media"), ("system", "Desktop Tools"),
                             ("users", "Local users"), ("games", "Game Library"),
                             ("custom", "Custom Sections"), ("friends", "Friends"),
                             ("account", "Local Account"), ("trophies", "Local trophies")):
            with Session(self, f"panel-{panel}", ["--open-panel", panel]) as session:
                session.wait(lambda image, title=title: self.panel_ready(image, title), "native-panel")
                if panel == "system" and self.native:
                    session.key("enter")
                    deadline = time.monotonic() + self.args.timeout
                    screenshots = []
                    while time.monotonic() < deadline:
                        screenshots = list((session.state_root / "data/OpenXMB/screenshots").glob("*.png"))
                        if screenshots:
                            break
                        session.frame()
                        time.sleep(0.1)
                    if not screenshots:
                        raise SmokeFailure("The native screenshot action did not create a PNG")
                    with Image.open(screenshots[-1]) as screenshot:
                        if foreground(screenshot) < 0.001:
                            raise SmokeFailure("The desktop screenshot omitted the native foreground")
                    session.key("down")
                    session.key("enter")
                    startup = session.state_root / "config/autostart/openxmb.desktop"
                    session.wait_path(startup)
                    if not startup.is_file() or "Exec=" not in startup.read_text():
                        raise SmokeFailure("The native startup-at-login action did not persist its desktop entry")
                    session.key("enter")
                    session.wait_path(startup, present=False)
                    if startup.exists():
                        raise SmokeFailure("The native startup-at-login action could not be disabled")
                    session.wait(lambda image: self.panel_ready(image, title), "screenshot-and-autostart-actions")

    def settings(self):
        state = None
        with Session(self, "settings", ["--open-panel", "settings"]) as session:
            state = session.state_root
            session.wait(lambda image: self.panel_ready(image, "OpenXMB Settings"), "settings-native-panel")
            session.key("down", 10)
            session.key("enter")
            session.wait_file(session.configuration, lambda config: config.get("render", {}).get("max-fps") == 60)
            session.wait(lambda image: foreground(image) > 0.001, "settings-frame-cap-saved", delay=0.2)
        with Session(self, "settings-restarted", ["--open-panel", "settings"], reuse=state) as session:
            session.wait(lambda image: self.panel_ready(image, "OpenXMB Settings"), "settings-reloaded")
            if json.loads(session.configuration.read_text())["render"]["max-fps"] != 60:
                raise SmokeFailure("The frame-cap setting did not survive restart")

    def themes(self):
        state = None
        source_hash = hashlib.sha256(self.fixtures.theme.read_bytes()).hexdigest()
        with Session(self, "themes", ["--open-panel", "themes"]) as session:
            state = session.state_root
            session.wait(lambda image: self.panel_ready(image, "Theme Library"), "theme-library")
            session.key("down")
            session.key("enter")
            session.wait(lambda image: foreground(image, (0.12, 0.36, 0.88, 0.78)) > 0.01, "theme-import-native-keyboard")
            inbox = state / "data/OpenXMB/themes/inbox"
            session.key("backspace", len(str(inbox)) + 5)
            session.type(str(self.fixtures.theme))
            session.key("enter")
            session.wait(lambda image: color_ratio(image, MAGENTA, 40, (0.55, 0.2, 0.94, 0.72)) > 0.10,
                         "theme-import-preview")
            session.key("enter")
            active_path = state / "data/OpenXMB/themes/active.json"
            session.wait_file(active_path, lambda saved: bool(saved.get("active", {}).get("id")))
            session.key("escape")
            session.wait(lambda image: color_ratio(image, MAGENTA, 35) > 0.55 and color_ratio(image, CYAN, 100) > 0.0015,
                         "theme-applied-background-and-icons")
            saved = json.loads(session.configuration.read_text())
            if saved.get("shell", {}).get("background-type") != "image":
                raise SmokeFailure("Applying the theme did not persist the actual image background")
        with Session(self, "themes-restarted", [], reuse=state) as session:
            session.wait(lambda image: color_ratio(image, MAGENTA, 35) > 0.55 and color_ratio(image, CYAN, 100) > 0.0015,
                         "theme-restored-after-restart")
        with Session(self, "themes-revert", ["--open-panel", "themes"], reuse=state) as session:
            session.wait(lambda image: self.panel_ready(image, "Theme Library"), "theme-library-reopened")
            session.key("enter")
            session.wait_file(state / "data/OpenXMB/themes/active.json", lambda saved: "active" not in saved)
            session.key("escape")
            session.wait(lambda image: color_ratio(image, MAGENTA, 35) < 0.05 and foreground(image) > 0.001,
                         "theme-original-appearance-restored")
        if hashlib.sha256(self.fixtures.theme.read_bytes()).hexdigest() != source_hash:
            raise SmokeFailure("Theme import modified its original archive")

    def run(self):
        if not self.app.is_file():
            raise SmokeFailure(f"Build the native application first: {self.app}", 2)
        if self.assets is None:
            raise SmokeFailure("Build/install shell assets first or pass --assets", 2)
        help_text = checked([str(self.app), "--help"], timeout=20)
        if "--open-media" not in help_text or "--open-panel" not in help_text:
            raise SmokeFailure("The executable is stale: it does not expose the new native media/panel routes", 2)
        cases = {"root": self.root, "photo": self.photo, "music": self.music, "video": self.video,
                 "pdf": lambda: self.document("pdf", self.fixtures.pdf, PDF_FIRST),
                 "comic": lambda: self.document("comic", self.fixtures.comic, COMIC_FIRST),
                 "panels": self.panels, "settings": self.settings, "themes": self.themes}
        failed_code = 0
        for name in self.args.cases.split(","):
            if name not in cases:
                raise SmokeFailure(f"Unknown case {name!r}; choose {','.join(cases)}", 2)
            started = time.monotonic()
            session_start = len(self.sessions)
            try:
                cases[name]()
                result = {"case": name, "status": "passed", "seconds": round(time.monotonic() - started, 3)}
                print(f"PASS {name}", flush=True)
            except (SmokeFailure, OSError, subprocess.TimeoutExpired) as error:
                result = {"case": name, "status": "failed", "error": str(error), "seconds": round(time.monotonic() - started, 3)}
                application_exit = next((entry["application_exit"] for entry in self.sessions[session_start:]
                    if entry["application_exit"] and not entry.get("terminated_for_cleanup")), None)
                error_code = (application_exit if application_exit > 0 else 128 - application_exit) if application_exit else getattr(error, "exit_code", 1)
                failed_code = failed_code or error_code
                print(f"FAIL {name}: {error}", flush=True)
            self.results.append(result)
            self.report()
            if failed_code and not self.args.keep_going:
                break
        if hashlib.sha256((ROOT / "config.json").read_bytes()).hexdigest() != self.default_hash:
            raise SmokeFailure("The native UI smoke changed the tracked default configuration")
        self.report()
        print(f"Native desktop smoke results: {self.output / 'report.json'}", flush=True)
        return failed_code

    def report(self):
        json_write(self.output / "report.json", {"application": str(self.app), "output": str(self.output),
            "resolution": [self.args.width, self.args.height], "text_ocr": bool(self.ocr), "native_window": bool(self.native),
            "cases": self.results, "sessions": self.sessions,
            "limits": ["Software Vulkan is a correctness check, not a hardware performance benchmark.",
                       "Browser panels and owned-window input have separate checks; browser page loading is not claimed here.",
                       "Physical and encrypted optical media require device testing."]})


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", type=Path, default=ROOT / "build/default/XMS.bin")
    parser.add_argument("--assets", type=Path)
    parser.add_argument("--output", type=Path, help="New directory to create; existing directories are never reused")
    parser.add_argument("--cases", default="root,photo,music,video,pdf,comic,settings,themes,panels")
    parser.add_argument("--timeout", type=float, default=60, help="Seconds allowed for each visible readiness assertion")
    parser.add_argument("--width", type=int, default=960)
    parser.add_argument("--height", type=int, default=540)
    parser.add_argument("--no-ocr", action="store_true", help="Use color/motion assertions without optional visible-text OCR")
    parser.add_argument("--require-ocr", action="store_true")
    parser.add_argument("--keep-going", action="store_true")
    parser.add_argument("--native-window", action="store_true", help="Use a real Xvfb/X11 SDL window, XTest keys and OS screenshots")
    parser.add_argument("--under-xvfb", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--generate-only", action="store_true", help="Generate and verify fresh fixtures without starting the app")
    args = parser.parse_args()
    if args.native_window and not args.generate_only:
        if sys.platform != "linux":
            parser.error("--native-window currently targets Linux X11; use the offscreen runner on other platforms")
        if not shutil.which("import"):
            parser.error("--native-window requires ImageMagick import")
        if not args.under_xvfb:
            if not shutil.which("xvfb-run"):
                parser.error("--native-window requires xvfb-run for an isolated display")
            os.execvp("xvfb-run", ["xvfb-run", "-a", "-s", f"-screen 0 {args.width}x{args.height}x24", sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], "--under-xvfb"])
        if not os.environ.get("DISPLAY"):
            parser.error("The isolated Xvfb display did not start")
    for tool in ("ffmpeg", "ffprobe"):
        if not shutil.which(tool):
            parser.error(f"{tool} is required")
    try:
        runner = Runner(args)
        if args.generate_only:
            runner.report()
            print(f"Generated verified native fixtures: {runner.fixtures.directory}")
            return 0
        return runner.run()
    except SmokeFailure as error:
        print(f"desktop-smoke: {error}", file=sys.stderr)
        return error.exit_code
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"desktop-smoke: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
