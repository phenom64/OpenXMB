#!/usr/bin/env python3
"""Build flat session action glyphs from native vector geometry (Pillow, NumPy)."""
import math
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / 'assets/shell/icons/actions'
SCALE = 4

# Quit Game: a ring split by a diagonal channel; each half is closed by a straight
# chord, giving two point-symmetric outlined circular segments. Units: 128 px canvas.
QUIT_CENTRE = 64.0
QUIT_OUTER_RADIUS = 42.0
QUIT_STROKE = 7.6  # 0.181 x radius, least-squares fit to the reference inset
QUIT_HALF_GAP = 5.3  # 0.127 x radius, channel half-width between the chords
QUIT_CHORD_ANGLE = 60.0  # degrees above horizontal, rising to the right
QUIT_SAMPLES = 8  # per axis per pixel; coverage evaluated at sample centres


def quit_segments():
    """Yield each half as ((radius, chord offset) of the filled segment,
    (radius, chord offset) of its hole, normal angle in degrees clockwise from +x)."""
    inner = QUIT_OUTER_RADIUS - QUIT_STROKE
    for normal in (90 - QUIT_CHORD_ANGLE, 270 - QUIT_CHORD_ANGLE):
        yield (QUIT_OUTER_RADIUS, QUIT_HALF_GAP), (inner, QUIT_HALF_GAP + QUIT_STROKE), normal


def quit_raster():
    size = 128 * QUIT_SAMPLES
    axis = (np.arange(size) + 0.5) / QUIT_SAMPLES - QUIT_CENTRE
    x, y = np.meshgrid(axis, axis)
    r = np.hypot(x, y)
    ink = np.zeros((size, size), bool)
    for (outer, outer_offset), (hole, hole_offset), normal in quit_segments():
        a = math.radians(normal)
        s = x * math.cos(a) + y * math.sin(a)
        ink |= (r <= outer) & (s >= outer_offset) & ~((r < hole) & (s > hole_offset))
    rgba = np.full((128, 128, 4), 255, np.uint8)  # white colour under all alpha: no dark fringes
    coverage = ink.reshape(128, QUIT_SAMPLES, 128, QUIT_SAMPLES).mean(axis=(1, 3))
    rgba[..., 3] = np.rint(coverage * 255).astype(np.uint8)
    return Image.fromarray(rgba, 'RGBA')


def quit_svg():
    def point(radius, angle):
        a = math.radians(angle)
        return (f'{QUIT_CENTRE + radius * math.cos(a):.3f} '
                f'{QUIT_CENTRE + radius * math.sin(a):.3f}')
    paths = []
    for *rings, normal in quit_segments():
        d = []
        for radius, offset in rings:
            half = math.degrees(math.acos(offset / radius))
            d.append(f'M{point(radius, normal - half)}A{radius:g} {radius:g} 0 0 1 '
                     f'{point(radius, normal + half)}Z')
        paths.append(f'<path fill-rule="evenodd" d="{"".join(d)}"/>')
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" '
            'viewBox="0 0 128 128" fill="#fff">' + ''.join(paths) + '</svg>\n')



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

    quit_raster().save(OUTPUT / 'quit_session.png')
    (OUTPUT / 'quit_session.svg').write_text(quit_svg(), encoding='utf-8', newline='\n')


if __name__ == '__main__':
    build()
