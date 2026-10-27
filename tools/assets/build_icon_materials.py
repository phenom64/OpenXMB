#!/usr/bin/env python3
"""Bake neutral icon lighting and bevels (Pillow and NumPy, offline only).

RGBA stores the outward normal XY, neutral display brightness and coverage.
The renderer tints this cached material and refracts the live background; it
needs no per-frame lighting textures, blur or specular-light calculations.
"""
from pathlib import Path
import argparse
import math
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


def direction(x, z):
    x, z = math.radians(x), math.radians(z)
    return np.array([math.sin(z)*math.cos(x), math.sin(z)*math.sin(x), math.cos(z)])


def bake(source: Path, target: Path):
    original = Image.open(source).convert('RGBA')
    scale = min(SIZE/original.width, SIZE/original.height)
    image = original.resize((round(original.width*scale), round(original.height*scale)), Image.Resampling.BILINEAR)
    canvas = Image.new('RGBA', (SIZE, SIZE))
    canvas.paste(image, ((SIZE-image.width)//2, (SIZE-image.height)//2))
    alpha = np.asarray(canvas.getchannel('A'), dtype=np.float32)/255
    height = box_blur(alpha, round(4*SIZE/96))
    padded = np.pad(height, 1, mode='edge')
    strength = 2.2*SIZE/96
    nx = -(padded[1:-1, 2:] - padded[1:-1, :-2])*0.5*strength
    ny = -(padded[2:, 1:-1] - padded[:-2, 1:-1])*0.5*strength
    norm = np.maximum(1, np.sqrt(nx*nx+ny*ny))
    nx, ny = nx/norm, ny/norm
    nz = np.sqrt(np.maximum(0, 1-nx*nx-ny*ny))
    normal = np.stack((nx, ny, nz), axis=2)
    y, x = np.mgrid[:SIZE, :SIZE]
    eye = np.stack((-278-((x+0.5)/SIZE-0.5)*37.485,
                    -299-((y+0.5)/SIZE-0.5)*37.485, np.full((SIZE, SIZE), 1200)), axis=2)
    eye /= np.linalg.norm(eye, axis=2, keepdims=True)
    weight = 0.6669999957084656+(1-0.6669999957084656)*np.power(nz, 0.416622)
    luminance = 1+(0.85-1)*weight
    remap = luminance.copy()
    anchors = [(0.2044299989938736, -0.7366139888763428),
               (0.5444409847259521, -0.2599810063838959), (1, 0)]
    for i, j, k in ((0, 1, 2), (1, 0, 2), (2, 0, 1)):
        x0, y0 = anchors[i]
        a, b = anchors[j][0], anchors[k][0]
        remap += (luminance-a)*(luminance-b)*(y0*0.0624733/((x0-a)*(x0-b)))
    lambert = np.zeros((SIZE, SIZE))
    specular = np.zeros((SIZE, SIZE))
    for light, power, gain in ((direction(92.4901, 22.4976), 44.8563, 0.484729),
                               (direction(84.9909, 282.47), 74.1114, 2.45394)):
        diffuse = np.maximum(0, np.sum(normal*light, axis=2))
        lambert += diffuse
        half = eye+light
        half /= np.linalg.norm(half, axis=2, keepdims=True)
        highlight = np.maximum(0, np.sum(normal*half, axis=2))
        specular += np.where(diffuse > 0, np.power(highlight, power)*gain, 0)
    linear = remap*(1.05025+lambert*3.12367)+specular
    brightness = 0.8*(1-np.power(2, -linear*1.1574*1.4429434537887573))
    material = np.stack((nx*0.5+0.5, ny*0.5+0.5, brightness, alpha), axis=2)
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
