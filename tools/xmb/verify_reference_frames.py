#!/usr/bin/env python3
"""Verify local reference frames match visual-thresholds.json SHA-256 pins."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--reference-dir",
        type=Path,
        default=Path("tests/xmb/reference_frames"),
    )
    parser.add_argument(
        "--thresholds",
        type=Path,
        default=Path("assets/xmb/manifests/visual-thresholds.json"),
    )
    args = parser.parse_args()

    thresholds = json.loads(args.thresholds.read_text(encoding="utf-8"))
    if thresholds.get("schema") != 1:
        raise ValueError(f"unsupported threshold schema in {args.thresholds}")

    failures = 0
    for scene, scene_data in thresholds["scenes"].items():
        expected = str(scene_data["reference_sha256"]).lower()
        path = args.reference_dir / f"{scene}.png"
        if not path.is_file():
            print(f"FAIL: missing reference frame {path}", file=sys.stderr)
            failures += 1
            continue
        actual = sha256(path)
        if actual != expected:
            print(
                f"FAIL: {scene}: expected {expected}, found {actual}",
                file=sys.stderr,
            )
            failures += 1
            continue
        print(f"OK: {scene} ({path})")

    return 1 if failures else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"verify_reference_frames: {error}", file=sys.stderr)
        raise SystemExit(2) from error