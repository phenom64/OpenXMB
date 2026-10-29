#!/usr/bin/env python3
"""Build flat session action glyphs from native vector geometry (Pillow)."""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / 'assets/shell/icons/actions'
SCALE = 4


def build():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    white = (255, 255, 255, 255)
    screenshot = Image.new('RGBA', (128 * SCALE, 128 * SCALE))
    draw = ImageDraw.Draw(screenshot)
    draw.rounded_rectangle(tuple(v * SCALE for v in (22, 29, 106, 99)),
        radius=12 * SCALE, outline=white, width=8 * SCALE)
    for rect in ((51, 23, 77, 40), (51, 88, 77, 105), (16, 56, 33, 72), (95, 56, 112, 72)):
        draw.rectangle(tuple(v * SCALE for v in rect), fill=(0, 0, 0, 0))
    screenshot.resize((128, 128), Image.Resampling.LANCZOS).save(OUTPUT / 'screenshot.png')

    quit_session = Image.new('RGBA', (128 * SCALE, 128 * SCALE))
    draw = ImageDraw.Draw(quit_session)
    draw.arc(tuple(v * SCALE for v in (25, 26, 103, 108)), 305, 235,
        fill=white, width=9 * SCALE)
    draw.line((64 * SCALE, 17 * SCALE, 64 * SCALE, 70 * SCALE), fill=white, width=9 * SCALE)
    quit_session.rotate(-30, Image.Resampling.BICUBIC).resize((128, 128),
        Image.Resampling.LANCZOS).save(OUTPUT / 'quit_session.png')


if __name__ == '__main__':
    build()
