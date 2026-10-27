#!/usr/bin/env python3
"""Bake translucent icon lighting and bevels (Pillow and NumPy, offline only).

RGBA stores the lit body, white gloss, glass opacity and silhouette coverage.
The renderer tints this cached material over the live background; it
needs no per-frame lighting textures, blur or specular-light calculations.
"""
from pathlib import Path
import argparse
import numpy as np
from PIL import Image

SIZE = 192


def box_blur(field, radius, passes=2):
    width = 2 * radius + 1
    for _ in range(passes):
        for axis in (1, 0):
            padding = [(0, 0), (0, 0)]
            padding[axis] = (radius, radius)
            data = np.pad(field, padding, mode='edge')
            sums = np.cumsum(data, axis=axis, dtype=np.float64)
            padding[axis] = (1, 0)
            sums = np.pad(sums, padding, mode='constant')
            if axis == 1:
                field = ((sums[:, width:] - sums[:, :-width]) / width).astype(np.float32)
            else:
                field = ((sums[width:] - sums[:-width]) / width).astype(np.float32)
    return field


def bake(source: Path, target: Path):
    original = Image.open(source).convert('RGBA')
    scale = min(SIZE/original.width, SIZE/original.height)
    image = original.resize((round(original.width*scale), round(original.height*scale)), Image.Resampling.BILINEAR)
    canvas = Image.new('RGBA', (SIZE, SIZE))
    canvas.paste(image, ((SIZE-image.width)//2, (SIZE-image.height)//2))
    alpha = np.asarray(canvas.getchannel('A'), dtype=np.float32)/255
    silhouette = np.argwhere(alpha > 0.15)
    glyph = max(8, int(np.ptp(silhouette, axis=0).max())) if silhouette.size else 8
    radius = max(1, int(np.floor(glyph*0.028+0.5)))
    height = box_blur(alpha, radius)
    padded = np.pad(height, 1, mode='edge')
    strength = 1.6/max(0.05, 1/(2*radius+1))
    nx = -(padded[1:-1, 2:] - padded[1:-1, :-2])*0.5*strength
    ny = -(padded[2:, 1:-1] - padded[:-2, 1:-1])*0.5*strength
    norm = np.maximum(1, np.hypot(nx, ny)/0.97)
    nx, ny = nx/norm, ny/norm
    nz = np.sqrt(np.maximum(0, 1-nx*nx-ny*ny))
    rim = (1-nz)**2
    body = np.where(ny < 0, 0.87+ny*0.49,
                    0.87+np.minimum(ny, 0.3)*0.1-np.maximum(0, ny-0.3)*0.5)
    gloss = np.exp(-((nx-0.27)/0.16)**2)*np.minimum(1, rim*6)*0.45
    opacity = np.minimum(1, 0.45+0.45*rim)
    lit = body*1.28*(0.7+0.3*rim)*np.minimum(opacity*2, 1)
    material = np.stack((lit, gloss, opacity, alpha), axis=2)
    encoded = np.floor(np.clip(material, 0, 1)*255+0.5).astype(np.uint8)
    target.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(encoded).save(target, optimize=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', type=Path, default=Path('assets/shell'))
    args = parser.parse_args()
    count = 0
    for source in sorted((args.assets/'icons').rglob('*.png')):
        # Flat controls and format badges stay on the ordinary image path.
        if source.relative_to(args.assets/'icons').parts[0] in ('osk', 'system', 'vp', 'ip'):
            continue
        bake(source, args.assets/'normalmaps'/source.relative_to(args.assets/'icons'))
        count += 1
    print(f'Baked {count} icon materials')


if __name__ == '__main__':
    main()
