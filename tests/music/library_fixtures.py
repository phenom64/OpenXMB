import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--ffmpeg", required=True)
parser.add_argument("--player", required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="openxmb-library-fixtures-") as directory:
    root = pathlib.Path(directory)
    (root / "nested").mkdir()
    for index in range(3):
        target = root / ("nested" if index else ".") / f"{index}.flac"
        group = "A" if index < 2 else "B"
        subprocess.run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                        f"sine=frequency=440:duration={0.1 + index * 0.05}", "-c:a", "flac",
                        "-metadata", f"title=Song {index}", "-metadata", f"artist=Artist {group}",
                        "-metadata", f"album=Album {group}", str(target)], check=True)
    (root / "ignore.txt").write_text("not audio")
    subprocess.run([args.player, str(root)], check=True)
