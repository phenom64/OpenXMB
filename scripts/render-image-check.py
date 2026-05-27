#!/usr/bin/env python3
"""Small dependency-free pixel checks for render smoke screenshots."""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path
from typing import Iterable


class ImageError(RuntimeError):
    pass


def paeth(left: int, up: int, upper_left: int) -> int:
    p = left + up - upper_left
    pa = abs(p - left)
    pb = abs(p - up)
    pc = abs(p - upper_left)
    if pa <= pb and pa <= pc:
        return left
    if pb <= pc:
        return up
    return upper_left


def png_rows(path: Path) -> tuple[int, int, int, list[bytes], list[tuple[int, int, int, int]], bytes]:
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ImageError(f"{path} is not a PNG file")

    width = height = bit_depth = color_type = interlace = None
    palette: list[tuple[int, int, int, int]] = []
    transparency = b""
    idat: list[bytes] = []
    pos = 8

    while pos < len(data):
        if pos + 8 > len(data):
            raise ImageError("truncated PNG chunk header")
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        chunk_type = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length

        if len(chunk) != length:
            raise ImageError(f"truncated PNG chunk {chunk_type!r}")
        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
        elif chunk_type == b"PLTE":
            palette = [(chunk[i], chunk[i + 1], chunk[i + 2], 255) for i in range(0, len(chunk), 3)]
        elif chunk_type == b"tRNS":
            transparency = chunk
        elif chunk_type == b"IDAT":
            idat.append(chunk)
        elif chunk_type == b"IEND":
            break

    if None in (width, height, bit_depth, color_type, interlace):
        raise ImageError("PNG is missing IHDR")
    if bit_depth != 8:
        raise ImageError(f"unsupported PNG bit depth {bit_depth}; expected 8")
    if interlace != 0:
        raise ImageError("interlaced PNGs are not supported by this smoke checker")

    channels_by_type = {
        0: 1,  # grayscale
        2: 3,  # truecolor
        3: 1,  # indexed
        4: 2,  # grayscale + alpha
        6: 4,  # truecolor + alpha
    }
    if color_type not in channels_by_type:
        raise ImageError(f"unsupported PNG color type {color_type}")

    channels = channels_by_type[color_type]
    stride = width * channels
    raw = zlib.decompress(b"".join(idat))
    rows: list[bytes] = []
    prev = bytearray(stride)
    raw_pos = 0

    for _ in range(height):
        if raw_pos >= len(raw):
            raise ImageError("truncated PNG scanline data")
        filter_type = raw[raw_pos]
        raw_pos += 1
        scanline = raw[raw_pos:raw_pos + stride]
        raw_pos += stride
        if len(scanline) != stride:
            raise ImageError("truncated PNG scanline")

        recon = bytearray(stride)
        for i, value in enumerate(scanline):
            left = recon[i - channels] if i >= channels else 0
            up = prev[i]
            upper_left = prev[i - channels] if i >= channels else 0

            if filter_type == 0:
                recon[i] = value
            elif filter_type == 1:
                recon[i] = (value + left) & 0xFF
            elif filter_type == 2:
                recon[i] = (value + up) & 0xFF
            elif filter_type == 3:
                recon[i] = (value + ((left + up) // 2)) & 0xFF
            elif filter_type == 4:
                recon[i] = (value + paeth(left, up, upper_left)) & 0xFF
            else:
                raise ImageError(f"unsupported PNG filter type {filter_type}")

        rows.append(bytes(recon))
        prev = recon

    if color_type == 3 and transparency:
        palette = [
            (r, g, b, transparency[i] if i < len(transparency) else a)
            for i, (r, g, b, a) in enumerate(palette)
        ]

    return width, height, color_type, rows, palette, transparency


def png_pixels(path: Path) -> tuple[int, int, Iterable[tuple[int, int, int, int]]]:
    width, height, color_type, rows, palette, transparency = png_rows(path)

    def iter_pixels() -> Iterable[tuple[int, int, int, int]]:
        for row in rows:
            if color_type == 0:
                for gray in row:
                    yield gray, gray, gray, 255
            elif color_type == 2:
                for i in range(0, len(row), 3):
                    yield row[i], row[i + 1], row[i + 2], 255
            elif color_type == 3:
                for index in row:
                    if index >= len(palette):
                        yield 0, 0, 0, 0
                    else:
                        yield palette[index]
            elif color_type == 4:
                for i in range(0, len(row), 2):
                    gray = row[i]
                    yield gray, gray, gray, row[i + 1]
            elif color_type == 6:
                for i in range(0, len(row), 4):
                    yield row[i], row[i + 1], row[i + 2], row[i + 3]

    return width, height, iter_pixels()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="PNG screenshot to inspect")
    parser.add_argument("--min-nonblank-ratio", type=float, default=0.01)
    parser.add_argument("--blank-threshold", type=int, default=8)
    parser.add_argument("--require-yellow", action="store_true")
    parser.add_argument("--min-yellow-pixels", type=int, default=1)
    parser.add_argument("--yellow-min-r", type=int, default=160)
    parser.add_argument("--yellow-min-g", type=int, default=120)
    parser.add_argument("--yellow-max-b", type=int, default=120)
    parser.add_argument("--yellow-min-rb-delta", type=int, default=80)
    parser.add_argument("--yellow-min-gb-delta", type=int, default=50)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        width, height, pixels = png_pixels(args.image)
    except (OSError, ImageError, zlib.error) as exc:
        print(f"render-image-check: {exc}", file=sys.stderr)
        return 2

    total = width * height
    nonblank = 0
    yellow = 0

    for r, g, b, a in pixels:
        visible = a > 0
        if visible and max(r, g, b) > args.blank_threshold:
            nonblank += 1
        if (
            visible
            and r >= args.yellow_min_r
            and g >= args.yellow_min_g
            and b <= args.yellow_max_b
            and (r - b) >= args.yellow_min_rb_delta
            and (g - b) >= args.yellow_min_gb_delta
        ):
            yellow += 1

    nonblank_ratio = nonblank / total if total else 0.0
    print(f"image={args.image}")
    print(f"size={width}x{height}")
    print(f"pixels={total}")
    print(f"nonblank_pixels={nonblank}")
    print(f"nonblank_ratio={nonblank_ratio:.6f}")
    print(f"yellow_pixels={yellow}")

    if nonblank_ratio < args.min_nonblank_ratio:
        print(
            "render-image-check: image is too blank "
            f"({nonblank_ratio:.6f} < {args.min_nonblank_ratio:.6f})",
            file=sys.stderr,
        )
        return 1
    if args.require_yellow and yellow < args.min_yellow_pixels:
        print(
            "render-image-check: not enough yellow probe pixels "
            f"({yellow} < {args.min_yellow_pixels})",
            file=sys.stderr,
        )
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
