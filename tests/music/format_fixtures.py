#!/usr/bin/env python3
"""Exercise real SDL decoders with freshly encoded, tagged music fixtures."""
import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--ffmpeg", required=True)
parser.add_argument("--player", required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="openxmb-music-formats-") as directory:
    root = pathlib.Path(directory)
    cover = root / "cover.jpg"
    subprocess.run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "color=c=blue:s=32x32", "-frames:v", "1", str(cover)], check=True)
    files = []
    for suffix, codec in [("mp3", "libmp3lame"), ("flac", "flac"), ("ogg", "libvorbis")]:
        target = root / ("tagged." + suffix)
        command = [args.ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=1.5"]
        if suffix == "mp3":
            command += ["-i", str(cover), "-map", "0:a", "-map", "1:v", "-c:v", "copy", "-disposition:v", "attached_pic", "-id3v2_version", "3"]
        command += ["-c:a", codec, "-metadata", "title=Fixture Title", "-metadata", "artist=Fixture Artist", str(target)]
        subprocess.run(command, check=True)
        files.append(str(target))
    subprocess.run([args.player, *files], check=True)
