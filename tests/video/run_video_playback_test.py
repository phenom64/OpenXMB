#!/usr/bin/env python3
"""Generate deterministic local media and exercise the native playback backend."""

import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--player", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    subtitle = args.output / "playback.srt"
    subtitle.write_text(
        "1\n00:00:00,150 --> 00:00:00,750\n<i>First line</i>\n\n"
        "2\n00:00:00,900 --> 00:00:01,850\nSecond line\n\n",
        encoding="utf-8",
    )
    environment = dict(os.environ)
    environment["SDL_AUDIODRIVER"] = "dummy"
    # The second fixture exercises delayed B-frame draining and planar audio
    # resampling, which lossless intra-frame video plus PCM cannot validate.
    fixtures = [
        ("playback", ["-c:v", "ffv1", "-c:a", "pcm_s16le"], 48000),
        ("playback_reordered", ["-c:v", "mpeg4", "-bf", "2", "-g", "12", "-c:a", "aac"], 44100),
    ]
    for name, codecs, sample_rate in fixtures:
        fixture = args.output / f"{name}.mkv"
        fixture.with_suffix(".srt").write_text(subtitle.read_text(encoding="utf-8"), encoding="utf-8")
        subprocess.run(
            [args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
             "-f", "lavfi", "-i", "testsrc2=size=96x64:rate=12:duration=2",
             "-f", "lavfi", "-i", f"sine=frequency=440:sample_rate={sample_rate}:duration=2",
             "-f", "lavfi", "-i", f"sine=frequency=880:sample_rate={sample_rate}:duration=2",
             "-i", str(subtitle), "-map", "0:v", "-map", "1:a", "-map", "2:a", "-map", "3:s",
             *codecs, "-pix_fmt", "yuv420p", "-c:s", "srt",
             "-metadata:s:a:0", "language=eng", "-metadata:s:a:1", "language=fra",
             "-metadata:s:s:0", "language=eng", str(fixture)],
            check=True,
        )
        status = subprocess.run([args.player, str(fixture)], env=environment).returncode
        if status:
            return status
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
