#!/usr/bin/env python3
"""Exercise FFmpeg streaming with deterministic, tagged codec fixtures."""
import argparse
import pathlib
import subprocess
import tempfile
import math
import struct

parser = argparse.ArgumentParser()
parser.add_argument("--ffmpeg", required=True)
parser.add_argument("--player", required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="openxmb-music-formats-") as directory:
    root = pathlib.Path(directory)
    cover = root / "cover.jpg"
    subprocess.run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "color=c=blue:s=32x32", "-frames:v", "1", str(cover)], check=True)
    files = []
    for suffix, codec in [("mp3", "libmp3lame"), ("flac", "flac"), ("ogg", "libvorbis"),
                          ("m4a", "aac"), ("m4a", "alac"), ("wav", "pcm_s16le"),
                          ("opus", "opus"), ("aiff", "pcm_s16be")]:
        target = root / (codec + "." + suffix)
        command = [args.ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=1.5"]
        if suffix == "mp3":
            command += ["-i", str(cover), "-map", "0:a", "-map", "1:v", "-c:v", "copy", "-disposition:v", "attached_pic", "-id3v2_version", "3"]
        command += ["-c:a", codec, "-metadata", "title=Fixture Title", "-metadata", "artist=Fixture Artist", str(target)]
        if codec == "opus":
            # The bundled FFmpeg exposes its native Opus encoder, not libopus.
            command[-1:-1] = ["-strict", "experimental", "-ar", "48000"]
        if suffix == "aiff":
            # Native AIFF chunks retain Title but require ID3 for Artist tags.
            command[-1:-1] = ["-write_id3v2", "1"]
        subprocess.run(command, check=True)
        files.append(str(target))
    # Original deterministic ProTracker module: one looping sine instrument,
    # one pattern, one note. No downloaded or copyrighted music fixture.
    module = bytearray(b"Fixture Title".ljust(20, b"\0"))
    for instrument in range(31):
        module += b"Sine".ljust(22, b"\0") + struct.pack(">HBBHH", 32 if instrument == 0 else 0, 0, 64, 0, 32 if instrument == 0 else 1)
    module += bytes([1, 0]) + bytes(128) + b"M.K."
    pattern = bytearray(1024)
    pattern[:4] = bytes([0x01, 0xAC, 0x10, 0x00])
    module += pattern + bytes(int(100 * math.sin(2 * math.pi * i / 64)) & 255 for i in range(64))
    tracker = root / "sine.mod"
    tracker.write_bytes(module)
    files.append(str(tracker))
    subprocess.run([args.player, *files], check=True)
