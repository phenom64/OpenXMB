#!/usr/bin/env python3
"""Generate format/range/odd-size fixtures and measure real frame preparation."""
import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--ffmpeg", required=True)
parser.add_argument("--player", required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="openxmb-native-video-") as directory:
    root = pathlib.Path(directory)
    cases = [
        ("yuv420p", "bt601", "smpte170m", "limited", "tv", "ffv1"),
        ("yuv420p", "bt709", "bt709", "limited", "tv", "ffv1"),
        ("yuv420p", "bt2020", "bt2020nc", "limited", "tv", "ffv1"),
        ("yuv420p", "bt709", "bt709", "full", "pc", "ffv1"),
        ("nv12", "bt709", "bt709", "limited", "tv", "rawvideo"),
        ("yuv444p", "bt709", "bt709", "limited", "tv", "ffv1"),
        ("yuv420p10le", "bt709", "bt709", "limited", "tv", "ffv1"),
    ]
    for index, (pixel_format, matrix, colorspace, range_name, color_range, codec) in enumerate(cases):
        # NUT rawvideo preserves NV12 storage rather than converting to planar.
        fixture = root / f"{index}.{'nut' if codec == 'rawvideo' else 'mkv'}"
        command = [args.ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                   "color=c=0x4c9cd1:size=96x64:rate=12:duration=0.25", "-vf", "crop=95:63:exact=1",
                   "-c:v", codec, "-pix_fmt", pixel_format, "-colorspace", colorspace, "-color_range", color_range,
                   str(fixture)]
        subprocess.run(command, check=True)
        native = pixel_format if pixel_format in ("yuv420p", "nv12") else "rgba"
        # Rawvideo in NUT has no color metadata: SD defaults to BT.601 limited.
        subprocess.run([args.player, str(fixture), native, "bt601" if codec == "rawvideo" else matrix, range_name], check=True)
    fixture = root / "benchmark.mkv"
    subprocess.run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                    "testsrc2=size=1920x1080:rate=24:duration=1.25", "-c:v", "ffv1", "-pix_fmt", "yuv420p",
                    "-colorspace", "bt709", str(fixture)], check=True)
    subprocess.run([args.player, str(fixture), "yuv420p", "bt709", "limited", "benchmark"], check=True)
