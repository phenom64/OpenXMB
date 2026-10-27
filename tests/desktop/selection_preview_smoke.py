#!/usr/bin/env python3
"""Render a generated game fixture and verify artwork plus changing video frames."""
import argparse
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def keyboard(process_id):
    source = Path(__file__).resolve().parents[2] / "scripts/desktop-smoke.py"
    spec = importlib.util.spec_from_file_location("native_desktop_input", source)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    native = module.NativeInput()
    require(native.locate(process_id), "OpenXMB test window was not created")

    def press(name):
        scan, symbol = module.KEYS[name.lower()]
        for state in ("down", "up"):
            native.line(process_id, f"keyboard {state} {scan} {symbol} 0")
        time.sleep(0.12)

    return press


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[2]
    app = args.app.resolve()
    if not os.environ.get("DISPLAY"):
        require(shutil.which("xvfb-run"), "This smoke check requires Xvfb or an existing X display")
        os.execvp("xvfb-run", ["xvfb-run", "-a", "-s", "-screen 0 960x540x24", sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]])
    with tempfile.TemporaryDirectory(prefix="openxmb-game-preview-") as temporary:
        root = args.output.resolve() if args.output else Path(temporary)
        root.mkdir(parents=True, exist_ok=True)
        data = root / "data"
        profile = data / "profiles/default"
        profile.mkdir(parents=True, exist_ok=True)
        (data / "profiles.json").write_text(json.dumps({"version": 1, "active_profile": "default", "profiles": [{"id": "default", "name": "Preview Fixture", "avatar": ""}]}))
        for name, color in [("background.png", "red"), ("icon.png", "lime")]:
            subprocess.run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", f"color=c={color}:s=320x180", "-frames:v", "1", str(root / name)], check=True)
        video = root / "preview.mkv"
        subprocess.run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "testsrc2=s=160x90:r=12:d=2", "-c:v", "ffv1", str(video)], check=True)
        music = root / "preview.wav"
        subprocess.run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "sine=frequency=440:duration=1", str(music)], check=True)
        game = {"id": "preview-fixture", "title": "Preview Fixture", "executable": "/bin/true", "arguments": [], "icon": str(root / "icon.png"), "background": str(root / "background.png"), "preview": str(video), "music": str(music)}
        (profile / "library.json").write_text(json.dumps({"version": 1, "games": [game]}))
        config = json.loads((source / "config.json").read_text())
        config.setdefault("render", {}).update({"sample-count": 1, "vsync": False, "max-fps": 30})
        config.setdefault("shell", {}).update({"background-type": "color", "font-path": "default"})
        config_file = root / "config.json"
        config_file.write_text(json.dumps(config))
        environment = dict(os.environ)
        environment.update({"XMB_ASSET_DIR": str(app.parent / "share/shell"), "XMB_LOCALE_DIR": str(app.parent / "locales"), "SDL_VIDEODRIVER": "x11", "AUROREENGINE_HEADLESS": "0", "OPENXMB_DATA_DIR": str(data), "OPENXMB_CONFIG": str(config_file), "OPENXMB_FIXED_BOOT_SECONDS": "100", "SDL_AUDIODRIVER": "dummy", "XDG_CACHE_HOME": str(root / "cache")})
        log = root / "app.log"
        with log.open("w") as output:
            process = subprocess.Popen([str(app), "--width", "960", "--height", "540", "--no-fullscreen", "--open-panel", "games"], env=environment, cwd=root, stdout=output, stderr=subprocess.STDOUT)
            try:
                time.sleep(2.5)
                require(process.poll() is None, "OpenXMB exited before game selection")
                press = keyboard(process.pid)
                for _ in range(3):
                    press("Down")
                time.sleep(2.0)
                shots = [root / "selection-one.png", root / "selection-two.png"]
                for shot in shots:
                    subprocess.run(["import", "-depth", "8", "-window", "root", str(shot)], check=True)
                    time.sleep(0.45)
                spec = importlib.util.spec_from_file_location("preview_image_check", source / "scripts/render-image-check.py")
                checker = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(checker)
                images = []
                for shot in shots:
                    width, height, pixels = checker.png_pixels(shot)
                    images.append((width, height, list(pixels)))
                width, height, first = images[0]
                _, _, second = images[1]
                require((width, height) == (960, 540), "Unexpected test display dimensions")
                red = sum(r > g + 40 and r > b + 40 for r, g, b, _ in [first[y * width + x] for y in range(80, 450) for x in range(8, 35)])
                green = sum(g > r + 40 and g > b + 40 for r, g, b, _ in [first[y * width + x] for y in range(350, 440) for x in range(730, 850)])
                changed = sum(sum(abs(a - b) for a, b in zip(first[y * width + x][:3], second[y * width + x][:3])) > 30 for y in range(175, 245) for x in range(665, 850))
                require(red > 1000, "Selected game background did not render")
                require(green > 500, "Selected game icon did not render")
                require(changed > 300, "Selected game preview did not publish changing decoded frames")
                print(f"Game selection preview passed: background_pixels={red}, icon_pixels={green}, changed_movie_pixels={changed}")
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    main()
