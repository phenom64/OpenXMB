#!/usr/bin/env python3
"""Bake bevel-normal textures from replaceable icon alpha masks (requires Pillow)."""
from pathlib import Path
import argparse
import math
from PIL import Image, ImageFilter

def bake(source: Path, target: Path):
    image = Image.open(source).convert('RGBA')
    image.thumbnail((192, 192), Image.Resampling.LANCZOS)
    alpha = image.getchannel('A')
    width, height = alpha.size
    surface = alpha.filter(ImageFilter.GaussianBlur(max(0.7, min(width, height) * 0.028)))
    pixels, mask = surface.load(), alpha.load()
    output = Image.new('RGBA', alpha.size)
    normal = output.load()
    gain = 2.2 * min(width, height) / 96
    for y in range(height):
        for x in range(width):
            nx = -(pixels[min(width-1,x+1), y] - pixels[max(0,x-1), y]) / 510 * gain
            ny = -(pixels[x, min(height-1,y+1)] - pixels[x, max(0,y-1)]) / 510 * gain
            length = math.hypot(nx, ny)
            if length > 0.98:
                nx, ny = nx * 0.98 / length, ny * 0.98 / length
            normal[x,y] = (round((nx+1)*127.5), round((ny+1)*127.5), 255, mask[x,y])
    target.parent.mkdir(parents=True, exist_ok=True)
    output.save(target, optimize=True)

def bake_lighting(target: Path):
    target.mkdir(parents=True, exist_ok=True)
    ambient = Image.new('RGB', (8, 128))
    for y in range(128):
        shade = round(80 + 160 * (1 - y / 127))
        for x in range(8): ambient.putpixel((x, y), (shade, shade, min(255, shade + 8)))
    ambient.save(target / 'icon_amb.png', optimize=True)
    environment = Image.new('RGB', (128, 128))
    for y in range(128):
        for x in range(128):
            highlight = math.exp(-(((x / 127 - .3) / .5)**2 + ((y / 127 - .23) / .24)**2))
            shade = round(30 + 180 * highlight)
            environment.putpixel((x, y), (shade, min(255, shade + 8), min(255, shade + 15)))
    environment.save(target / 'texenv.png', optimize=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', type=Path, default=Path('assets/shell'))
    args = parser.parse_args()
    bake_lighting(args.assets/'materials')
    for source in sorted((args.assets/'icons').rglob('*.png')):
        bake(source, args.assets/'normalmaps'/source.relative_to(args.assets/'icons'))
if __name__ == '__main__':
    main()
