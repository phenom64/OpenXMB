#!/usr/bin/env python3
"""Decorate OpenXMB's rendered purple wave with its packaged Play typography."""
import argparse
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wave', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    size = (900, 600)
    image = Image.open(args.wave).convert('RGBA').resize(size, Image.Resampling.LANCZOS)
    image = Image.blend(image, Image.new('RGBA', size, '#10051c'), 0.26)
    font = ImageFont.truetype(str(ROOT / 'interfaceFX/TypefaceServer/Play-Regular.ttf'), 72)
    label = Image.new('RGBA', size)
    draw = ImageDraw.Draw(label)
    draw.text((450, 85), 'openXMB', font=font, fill='#f9f4ff', anchor='mm')
    glow = label.filter(ImageFilter.GaussianBlur(10))
    image = Image.alpha_composite(image, glow)
    image = Image.alpha_composite(image, label.filter(ImageFilter.GaussianBlur(2)))
    image = Image.alpha_composite(image, label)
    mask = Image.new('L', size)
    ImageDraw.Draw(mask).polygon([(375, 302), (469, 302), (469, 280),
                                  (525, 320), (469, 360), (469, 338), (375, 338)], fill=230)
    shadow = Image.new('RGBA', size, '#08020e')
    shadow.putalpha(mask.filter(ImageFilter.GaussianBlur(9)))
    image = Image.alpha_composite(image, shadow)
    arrow = Image.new('RGBA', size)
    pixels = ImageDraw.Draw(arrow)
    for y in range(280, 361):
        amount = (y - 280) / 80
        value = round(245 - 105 * amount + (35 if 310 <= y <= 315 else 0))
        pixels.line((370, y, 530, y), fill=(min(255, value), min(255, value), 255, 255))
    arrow.putalpha(mask)
    rim = ImageChops.subtract(mask, mask.filter(ImageFilter.MinFilter(3)))
    highlight = Image.new('RGBA', size, '#f9f0ff')
    highlight.putalpha(rim)
    image = Image.alpha_composite(image, arrow)
    image = Image.alpha_composite(image, highlight)
    footer = Image.new('RGBA', size)
    ImageDraw.Draw(footer).text((450, 530), 'Drag OpenXMB into Applications',
        font=ImageFont.truetype(str(ROOT / 'interfaceFX/TypefaceServer/Play-Regular.ttf'), 22),
        fill='#eee0fa', anchor='mm')
    image = Image.alpha_composite(image, footer.filter(ImageFilter.GaussianBlur(3)))
    image = Image.alpha_composite(image, footer)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    image.convert('RGB').save(args.output)


if __name__ == '__main__':
    main()
