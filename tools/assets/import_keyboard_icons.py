#!/usr/bin/env python3
"""Prepare white, transparent SVG key glyphs from a checked-out icon pack."""
import argparse
from pathlib import Path
import re
import xml.etree.ElementTree as ET


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    ET.register_namespace('', 'http://www.w3.org/2000/svg')
    ET.register_namespace('xlink', 'http://www.w3.org/1999/xlink')
    files = sorted((args.source / 'icons').glob('icon-*.svg'))
    if len(files) != 65:
        raise SystemExit('Expected the complete 65-key vector pack.')
    args.output.mkdir(parents=True, exist_ok=True)
    for path in files:
        root = ET.fromstring(path.read_bytes())
        box = [float(value) for value in root.attrib['viewBox'].split()]
        root.set('fill', '#ffffff')
        # Preserve the viewBox and wide-key proportions. An explicit 4x viewport
        # gives the native SVG decoder enough samples for high-DPI prompts.
        root.set('width', f'{box[2] * 4:g}')
        root.set('height', f'{box[3] * 4:g}')
        for element in root.iter():
            for key, value in list(element.attrib.items()):
                element.set(key, re.sub(r'#(?:000000|010101|000)\b', '#ffffff', value, flags=re.I))
            if element.text:
                element.text = re.sub(r'#(?:000000|010101|000)\b', '#ffffff', element.text, flags=re.I)
        ET.ElementTree(root).write(args.output / path.name, encoding='utf-8', xml_declaration=True)
    print(f'Prepared {len(files)} transparent SVG keyboard glyphs.')


if __name__ == '__main__':
    main()
