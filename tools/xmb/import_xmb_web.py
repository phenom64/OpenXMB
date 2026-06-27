#!/usr/bin/env python3
"""Create the manifest-verified, local-only OpenXMB xmb-web compatibility pack."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
from typing import Any


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
SOURCE_MANIFEST = REPOSITORY_ROOT / "assets/xmb/manifests/xmb-web-source.json"
LFS_PREFIX = b"version https://git-lfs.github.com/spec/v1"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class ImportFailure(RuntimeError):
    pass


def canonical_json(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode(
        "utf-8"
    )


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_bytes(path: Path) -> bytes:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise ImportFailure(f"cannot read {path}: {error}") from error
    if data.startswith(LFS_PREFIX):
        raise ImportFailure(f"{path} is a Git LFS pointer, not asset content")
    return data


def is_within(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def git_commit(source: Path) -> str | None:
    git_marker = source / ".git"
    if not git_marker.exists():
        return None
    try:
        completed = subprocess.run(
            ["git", "-C", str(source), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError) as error:
        raise ImportFailure(f"Git metadata exists but its commit cannot be read: {error}") from error
    return completed.stdout.strip().lower()


def load_source_manifest() -> dict[str, Any]:
    try:
        manifest = json.loads(SOURCE_MANIFEST.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ImportFailure(f"cannot load source manifest {SOURCE_MANIFEST}: {error}") from error
    if manifest.get("schema_version") != 1 or not isinstance(manifest.get("assets"), list):
        raise ImportFailure("unsupported or malformed xmb-web source manifest")
    return manifest


def validate_pinned_file(source: Path, entry: dict[str, Any]) -> bytes:
    relative = entry.get("source_relative_path")
    if not isinstance(relative, str) or not relative or relative.startswith(("/", "\\")):
        raise ImportFailure(f"invalid source_relative_path for {entry.get('logical_id')!r}")
    path = (source / relative).resolve()
    if not is_within(path, source):
        raise ImportFailure(f"source path escapes checkout: {relative}")
    data = read_bytes(path)
    expected_bytes = entry.get("bytes")
    expected_hash = entry.get("sha256")
    if len(data) != expected_bytes:
        raise ImportFailure(
            f"{relative}: expected {expected_bytes} bytes, found {len(data)}"
        )
    actual_hash = sha256(data)
    if actual_hash != expected_hash:
        raise ImportFailure(
            f"{relative}: SHA-256 mismatch; expected {expected_hash}, found {actual_hash}"
        )
    return data


def validate_wave_formats(files: dict[str, bytes]) -> None:
    geometry = files["wave.geometry"]
    expected_geometry_bytes = 128 * 128 * 9 * 4
    if len(geometry) != expected_geometry_bytes:
        raise ImportFailure("wave_geo.bin is not a 128x128 grid of nine float32 values")
    # Spot every scalar: this detects non-finite/wrong-endian material before copying.
    for (value,) in struct.iter_unpack("<f", geometry):
        if not math.isfinite(value):
            raise ImportFailure("wave_geo.bin contains a non-finite little-endian float32")

    sequence = files["wave.idle.sequence"]
    if len(sequence) < 24:
        raise ImportFailure("wave_seq2.bin has a truncated header")
    magic, frames, grid, stride, fmt, z_w_ratio = struct.unpack_from("<4sIIIIf", sequence)
    sequence_json = json.loads(files["wave.idle.metadata"].decode("utf-8"))
    expected_sequence_bytes = 24 + frames * grid * grid * stride * 2
    if (
        magic != b"WSQ2"
        or frames != 85
        or grid != 128
        or stride != 3
        or fmt != 1
        or not math.isfinite(z_w_ratio)
        or len(sequence) != expected_sequence_bytes
        or sequence_json.get("magic") != "WSQ2"
        or sequence_json.get("frameCount") != frames
        or sequence_json.get("gridN") != grid
        or sequence_json.get("vertStride") != stride
        or sequence_json.get("headerBytes") != 24
        or sequence_json.get("frameBytes") != grid * grid * stride * 2
        or abs(float(sequence_json.get("zwRatio", 0.0)) - z_w_ratio) > 0.000001
    ):
        raise ImportFailure("wave_seq2.bin and wave_seq2.json are inconsistent")

    boot = files["wave.boot.sequence"]
    if len(boot) < 20:
        raise ImportFailure("wave_boot.bin has a truncated header")
    boot_magic, boot_frames, boot_grid, boot_stride, boot_fmt = struct.unpack_from(
        "<4sIIII", boot
    )
    boot_json = json.loads(files["wave.boot.metadata"].decode("utf-8"))
    expected_boot_bytes = 20 + boot_frames * boot_grid * boot_grid * boot_stride * 2
    keyframes = boot_json.get("keyframes")
    if (
        boot_magic != b"WBT1"
        or boot_frames != 42
        or boot_grid != 128
        or boot_stride != 4
        or boot_fmt != 1
        or len(boot) != expected_boot_bytes
        or boot_json.get("magic") != "WBT1"
        or boot_json.get("frameCount") != boot_frames
        or boot_json.get("gridN") != boot_grid
        or boot_json.get("vertStride") != boot_stride
        or boot_json.get("headerBytes") != 20
        or boot_json.get("frameBytes") != boot_grid * boot_grid * boot_stride * 2
        or not isinstance(keyframes, list)
        or len(keyframes) != boot_frames
    ):
        raise ImportFailure("wave_boot.bin and wave_boot.json are inconsistent")

    previous_frame = -1
    previous_time = -1.0
    source_fps = int(boot_json.get("sourceFps", 0))
    for index, keyframe in enumerate(keyframes):
        frame = keyframe.get("frame")
        boot_time = keyframe.get("boot_t")
        brightness = keyframe.get("brightness01")
        if (
            not isinstance(frame, int)
            or not isinstance(boot_time, (int, float))
            or not isinstance(brightness, (int, float))
            or frame <= previous_frame
            or boot_time <= previous_time
            or not 0.0 <= brightness <= 1.0
            or abs(boot_time - frame / source_fps) > 0.00011
        ):
            raise ImportFailure(f"wave_boot.json keyframe {index} is inconsistent")
        previous_frame, previous_time = frame, float(boot_time)

    normal_map = files["icon.normal.spike"]
    if len(normal_map) < 24 or not normal_map.startswith(PNG_SIGNATURE):
        raise ImportFailure("nmap_000.png is not a complete PNG")
    width, height = struct.unpack_from(">II", normal_map, 16)
    if (width, height) != (128, 128):
        raise ImportFailure("nmap_000.png must be 128x128")


MONTH_ENTRY_RE = re.compile(
    r"\{\s*h:\s*([-+]?\d+(?:\.\d+)?)\s*,\s*s:\s*([-+]?\d+(?:\.\d+)?)"
    r"\s*,\s*v:\s*([-+]?\d+(?:\.\d+)?)\s*,\s*nv:\s*([-+]?\d+(?:\.\d+)?)\s*\}"
)
BOTTOM_ENTRY_RE = re.compile(
    r"(\d+)\s*:\s*\{\s*h:\s*([-+]?\d+(?:\.\d+)?)\s*,\s*s:\s*([-+]?\d+(?:\.\d+)?)"
    r"\s*,\s*v:\s*([-+]?\d+(?:\.\d+)?)\s*,\s*nv:\s*([-+]?\d+(?:\.\d+)?)\s*\}"
)


def extract_month_table(index_html: bytes, commit: str) -> bytes:
    try:
        source = index_html.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ImportFailure(f"index.html is not UTF-8: {error}") from error
    table_match = re.search(
        r"const\s+MONTH_GRADIENT\s*=\s*\[(.*?)\];", source, re.DOTALL
    )
    bottom_match = re.search(
        r"const\s+MONTH_GRADIENT_BOT\s*=\s*\{(.*?)\};", source, re.DOTALL
    )
    precomp_match = re.search(
        r"(?:const|let|var)\s+MONTH_VALUE_PRECOMP\s*=\s*([-+]?\d+(?:\.\d+)?)\s*;",
        source,
    )
    if table_match is None or bottom_match is None or precomp_match is None:
        raise ImportFailure("index.html no longer exposes the audited month symbols")
    values = MONTH_ENTRY_RE.findall(table_match.group(1))
    if len(values) != 12:
        raise ImportFailure(f"expected 12 MONTH_GRADIENT entries, found {len(values)}")
    months = [
        {
            "hue_degrees": float(hue),
            "month": index + 1,
            "night_value": float(night),
            "saturation": float(saturation),
            "value": float(value),
        }
        for index, (hue, saturation, value, night) in enumerate(values)
    ]
    bottom_overrides = [
        {
            "hue_degrees": float(hue),
            "month": int(zero_based_month) + 1,
            "night_value": float(night),
            "saturation": float(saturation),
            "value": float(value),
        }
        for zero_based_month, hue, saturation, value, night in BOTTOM_ENTRY_RE.findall(
            bottom_match.group(1)
        )
    ]
    if bottom_overrides != [
        {
            "hue_degrees": 242.0,
            "month": 7,
            "night_value": 0.18,
            "saturation": 0.9,
            "value": 0.23,
        }
    ]:
        raise ImportFailure("the audited July bottom override changed unexpectedly")
    artifact = {
        "bottom_overrides": bottom_overrides,
        "months": months,
        "schema_version": 1,
        "source": {
            "commit": commit,
            "relative_path": "index.html",
            "symbols": [
                "MONTH_GRADIENT",
                "MONTH_GRADIENT_BOT",
                "MONTH_VALUE_PRECOMP",
            ],
        },
        "value_precompensation": float(precomp_match.group(1)),
    }
    return canonical_json(artifact)


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".xmb-import.tmp")
    try:
        temporary.write_bytes(data)
        os.replace(temporary, path)
    except OSError as error:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise ImportFailure(f"cannot write {path}: {error}") from error


def output_entry(source_entry: dict[str, Any], data: bytes) -> dict[str, Any]:
    return {
        "bytes": len(data),
        "fallback": source_entry["fallback"],
        "logical_id": source_entry["logical_id"],
        "provenance_classification": source_entry["provenance_classification"],
        "required_optional_tier": source_entry["required_optional_tier"],
        "role": source_entry["role"],
        "sha256": sha256(data),
        "source_relative_path": source_entry["source_relative_path"],
        "target_pack": source_entry["target_pack"],
        "target_relative_path": source_entry["target_relative_path"],
    }


def validate_asset_family(
    source: Path, specification: dict[str, Any]
) -> list[tuple[dict[str, Any], bytes]]:
    source_directory = specification.get("source_directory")
    target_directory = specification.get("target_directory")
    logical_prefix = specification.get("logical_id_prefix")
    extensions = specification.get("extensions")
    if not all(isinstance(value, str) and value for value in (
        source_directory, target_directory, logical_prefix
    )):
        raise ImportFailure("asset family has an invalid directory or logical prefix")
    if not isinstance(extensions, list) or not extensions:
        raise ImportFailure(f"asset family {logical_prefix!r} has no extension allowlist")
    allowed_extensions = {
        str(extension).lower() if str(extension).startswith(".")
        else "." + str(extension).lower()
        for extension in extensions
    }

    family_root = (source / source_directory).resolve()
    if not family_root.is_dir() or not is_within(family_root, source):
        raise ImportFailure(f"asset family source is missing or escapes checkout: {family_root}")

    paths = sorted(
        (
            path for path in family_root.rglob("*")
            if path.is_file() and path.suffix.lower() in allowed_extensions
        ),
        key=lambda path: path.relative_to(family_root).as_posix(),
    )
    if len(paths) != specification.get("expected_files"):
        raise ImportFailure(
            f"asset family {logical_prefix!r}: expected {specification.get('expected_files')} "
            f"files, found {len(paths)}"
        )

    imported: list[tuple[dict[str, Any], bytes]] = []
    aggregate = hashlib.sha256()
    total_bytes = 0
    for path in paths:
        if path.is_symlink():
            raise ImportFailure(f"asset family does not permit symlinks: {path}")
        relative_within_family = path.relative_to(family_root).as_posix()
        data = read_bytes(path)
        digest = sha256(data)
        total_bytes += len(data)
        aggregate.update(relative_within_family.encode("utf-8"))
        aggregate.update(b"\0")
        aggregate.update(digest.encode("ascii"))
        aggregate.update(b"\0")
        aggregate.update(str(len(data)).encode("ascii"))
        aggregate.update(b"\n")

        logical_suffix = re.sub(r"[^a-z0-9]+", ".", relative_within_family.lower()).strip(".")
        entry = {
            "fallback": specification["fallback"],
            "logical_id": f"{logical_prefix}.{logical_suffix}",
            "provenance_classification": specification["provenance_classification"],
            "required_optional_tier": specification["required_optional_tier"],
            "role": specification["role"],
            "source_relative_path": f"{source_directory}/{relative_within_family}",
            "target_pack": specification["target_pack"],
            "target_relative_path": f"{target_directory}/{relative_within_family}",
        }
        imported.append((entry, data))

    if total_bytes != specification.get("expected_bytes"):
        raise ImportFailure(
            f"asset family {logical_prefix!r}: expected {specification.get('expected_bytes')} "
            f"bytes, found {total_bytes}"
        )
    actual_aggregate = aggregate.hexdigest()
    if actual_aggregate != specification.get("aggregate_sha256"):
        raise ImportFailure(
            f"asset family {logical_prefix!r}: aggregate SHA-256 mismatch; "
            f"expected {specification.get('aggregate_sha256')}, found {actual_aggregate}"
        )
    return imported


def run(source: Path, output: Path, verify_only: bool) -> dict[str, Any]:
    manifest = load_source_manifest()
    source = source.resolve()
    output = output.resolve()
    if not source.is_dir():
        raise ImportFailure(f"source is not a directory: {source}")
    if output == source or is_within(output, source):
        raise ImportFailure("output must not be the xmb-web source checkout")

    expected_commit = manifest["source"]["commit"].lower()
    actual_commit = git_commit(source)
    if actual_commit is not None and actual_commit != expected_commit:
        raise ImportFailure(
            f"source commit mismatch: expected {expected_commit}, found {actual_commit}"
        )
    pinned_commit = actual_commit or expected_commit

    source_files: dict[str, bytes] = {}
    source_entries: dict[str, dict[str, Any]] = {}
    for entry in manifest["assets"]:
        logical_id = entry.get("logical_id")
        if not isinstance(logical_id, str) or logical_id in source_files:
            raise ImportFailure(f"invalid or duplicate logical ID: {logical_id!r}")
        source_files[logical_id] = validate_pinned_file(source, entry)
        source_entries[logical_id] = entry
    validate_wave_formats(source_files)

    month_spec = manifest["generated_assets"][0]
    index_entry = source_entries[month_spec["logical_id"]]
    month_data = extract_month_table(source_files[month_spec["logical_id"]], pinned_commit)
    if len(month_data) != month_spec["generated_bytes"] or sha256(month_data) != month_spec[
        "generated_sha256"
    ]:
        raise ImportFailure(
            "generated month table differs from the pinned audited machine-readable artifact"
        )

    imported: list[tuple[dict[str, Any], bytes]] = []
    for logical_id, data in source_files.items():
        entry = source_entries[logical_id]
        if entry.get("import"):
            imported.append((entry, data))
    month_entry = dict(index_entry)
    month_entry.update(
        {
            "logical_id": month_spec["output_logical_id"],
            "role": month_spec["role"],
            "target_relative_path": month_spec["target_relative_path"],
            "fallback": month_spec["fallback"],
        }
    )
    imported.append((month_entry, month_data))
    for family in manifest.get("asset_families", []):
        imported.extend(validate_asset_family(source, family))

    logical_ids = [entry["logical_id"] for entry, _ in imported]
    target_paths = [entry["target_relative_path"] for entry, _ in imported]
    if len(logical_ids) != len(set(logical_ids)):
        raise ImportFailure("compatibility pack contains duplicate logical IDs")
    if len(target_paths) != len(set(target_paths)):
        raise ImportFailure("compatibility pack contains duplicate target paths")
    generated_entries = sorted(
        (output_entry(entry, data) for entry, data in imported),
        key=lambda entry: entry["logical_id"],
    )
    output_manifest = {
        "assets": generated_entries,
        "pack_format": "openxmb-compat-loose-v1",
        "schema_version": 1,
        "source": {
            "commit": pinned_commit,
            "project": manifest["source"]["project"],
            "repository": manifest["source"]["repository"],
        },
    }
    manifest_data = canonical_json(output_manifest)

    if not verify_only:
        for entry, data in imported:
            target = (output / entry["target_relative_path"]).resolve()
            if not is_within(target, output):
                raise ImportFailure(f"target path escapes output: {target}")
            atomic_write(target, data)
        atomic_write(output / "manifest.json", manifest_data)
        for entry in generated_entries:
            target = output / entry["target_relative_path"]
            written = read_bytes(target)
            if len(written) != entry["bytes"] or sha256(written) != entry["sha256"]:
                raise ImportFailure(f"post-write verification failed for {target}")

    return {
        "asset_count": len(generated_entries),
        "asset_bytes": sum(entry["bytes"] for entry in generated_entries),
        "manifest_sha256": sha256(manifest_data),
        "mode": "verify-only" if verify_only else "import",
        "output": str(output),
        "source_commit": pinned_commit,
    }


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify/import the pinned local xmb-web OpenXMB compatibility pack"
    )
    parser.add_argument("--source", required=True, type=Path, help="pinned xmb-web checkout")
    parser.add_argument("--output", required=True, type=Path, help="build/temp pack directory")
    parser.add_argument(
        "--verify-only",
        action="store_true",
        help="validate source, formats, and generated manifest without writing output",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        summary = run(arguments.source, arguments.output, arguments.verify_only)
    except ImportFailure as error:
        print(f"xmb-web import failed: {error}", file=sys.stderr)
        return 2
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
