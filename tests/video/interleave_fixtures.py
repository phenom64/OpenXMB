"""Exercise bounded demux read-ahead using real badly interleaved codec packets."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--ffmpeg", required=True)
parser.add_argument("--player", required=True)
args = parser.parse_args()
environment = dict(os.environ, SDL_AUDIODRIVER="dummy")

with tempfile.TemporaryDirectory(prefix="openxmb-interleave-") as directory:
    root = Path(directory)
    for name, duration, codecs in [
        ("lossless", 6, ["-c:v", "ffv1", "-c:a", "pcm_s16le"]),
        ("reordered", 6, ["-c:v", "mpeg4", "-bf", "2", "-g", "24", "-c:a", "aac"]),
        ("limit", 45, ["-c:v", "ffv1", "-c:a", "pcm_s16le"]),
    ]:
        source, burst = root / f"{name}-source.mkv", root / f"{name}-burst.mkv"
        subprocess.run([
            args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
            "-f", "lavfi", "-i", f"testsrc2=size=96x64:rate=24:duration={duration}",
            "-f", "lavfi", "-i", f"sine=frequency=440:sample_rate=48000:duration={duration}",
            *codecs, "-pix_fmt", "yuv420p", str(source),
        ], check=True, env=environment)
        subprocess.run([args.player, "--remux", str(source), str(burst)], check=True, env=environment)
        subprocess.run([args.player, *(["--limit"] if name == "limit" else []), str(burst)], check=True, env=environment)
