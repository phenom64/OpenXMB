#!/usr/bin/env python3
"""Deterministically compare a native OpenXMB frame with a reference frame."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_rgb(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"), dtype=np.float32) / 255.0


def load_scene(path: Path, scene: str) -> tuple[dict[str, float], str]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("schema") != 1:
        raise ValueError(f"unsupported threshold schema in {path}")
    try:
        raw_scene = document["scenes"][scene]
        raw = raw_scene["thresholds"]
        return (
            {name: float(value) for name, value in raw.items()},
            str(raw_scene["reference_sha256"]).lower(),
        )
    except KeyError as error:
        raise ValueError(f"scene {scene!r} has no registered thresholds") from error


def compare(reference: np.ndarray, actual: np.ndarray) -> dict[str, float]:
    absolute = np.abs(reference - actual)
    squared = np.square(reference - actual)
    pixel_peak = np.max(absolute, axis=2)
    return {
        "mae": float(np.mean(absolute)),
        "rmse": float(np.sqrt(np.mean(squared))),
        "p95_absolute_error": float(np.percentile(absolute, 95)),
        "fraction_pixels_over_32": float(np.mean(pixel_peak > (32.0 / 255.0))),
        "maximum_absolute_error": float(np.max(absolute)),
    }


def write_heatmap(path: Path, reference: np.ndarray, actual: np.ndarray) -> None:
    # A black-to-red/yellow map makes small geometry offsets visible while
    # preserving a deterministic, dependency-light output artifact.
    peak = np.max(np.abs(reference - actual), axis=2)
    red = np.clip(peak * 4.0, 0.0, 1.0)
    green = np.clip((peak - 0.125) * 5.0, 0.0, 1.0)
    blue = np.zeros_like(peak)
    rendered = np.stack((red, green, blue), axis=2)
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.uint8(np.round(rendered * 255.0)), mode="RGB").save(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--actual", required=True, type=Path)
    parser.add_argument("--thresholds", required=True, type=Path)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--heatmap", type=Path)
    args = parser.parse_args()

    registered, expected_reference_hash = load_scene(args.thresholds, args.scene)
    reference_hash = sha256(args.reference)
    if reference_hash != expected_reference_hash:
        raise ValueError(
            f"reference hash for scene {args.scene!r} is {reference_hash}, "
            f"expected {expected_reference_hash}"
        )
    reference = load_rgb(args.reference)
    actual = load_rgb(args.actual)
    if reference.shape != actual.shape:
        raise ValueError(
            f"frame dimensions differ: reference={reference.shape[1]}x{reference.shape[0]}, "
            f"actual={actual.shape[1]}x{actual.shape[0]}"
        )

    metrics = compare(reference, actual)
    comparisons = {
        name: {
            "actual": metrics[name],
            "maximum": limit,
            "passed": metrics[name] <= limit,
        }
        for name, limit in registered.items()
    }
    passed = all(item["passed"] for item in comparisons.values())
    report: dict[str, Any] = {
        "schema": 1,
        "scene": args.scene,
        "passed": passed,
        "dimensions": {"width": int(reference.shape[1]), "height": int(reference.shape[0])},
        "reference": {"path": str(args.reference.resolve()), "sha256": reference_hash},
        "actual": {"path": str(args.actual.resolve()), "sha256": sha256(args.actual)},
        "metrics": metrics,
        "threshold_comparisons": comparisons,
    }

    serialized = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(serialized, encoding="utf-8")
    if args.heatmap:
        write_heatmap(args.heatmap, reference, actual)
    sys.stdout.write(serialized)
    return 0 if passed else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"compare_frames: {error}", file=sys.stderr)
        raise SystemExit(2) from error
