#!/usr/bin/env python3
"""Opt-in native rendering of a user's PS3 assets; never launches the emulator."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess


def metadata(path):
    data = path.read_bytes()
    assert len(data) <= 2 * 1024 * 1024
    magic, _, keys, values, count = struct.unpack_from('<5I', data)
    assert magic == 0x46535000 and count <= 512
    result = {}
    for index in range(count):
        key, kind, length, _, offset = struct.unpack_from('<HHIII', data, 20 + index * 16)
        if kind != 0x0204:
            continue
        name = data[keys + key:data.index(b'\0', keys + key)].decode('utf-8')
        result[name] = data[values + offset:values + offset + length].rstrip(b'\0').decode('utf-8')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--game-directory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    from PIL import Image
    app, game, root = args.app.resolve(), args.game_directory.resolve(), args.output.resolve()
    files = [game / name for name in ('PARAM.SFO', 'ICON0.PNG', 'PIC1.PNG', 'SND0.AT3')]
    before = {path: hashlib.sha256(path.read_bytes()).digest() for path in files}
    info = metadata(files[0])
    assert info.get('CATEGORY') in ('DG', 'HG')
    root.mkdir(parents=True, exist_ok=False)
    profile = root / 'data/profiles/default'
    profile.mkdir(parents=True)
    (root / 'data/profiles.json').write_text(json.dumps({'version': 1, 'active_profile': 'default',
        'profiles': [{'id': 'default', 'name': 'Native PS3 Media'}]}))
    entry = {'id': 'native-ps3-media', 'title': info['TITLE'], 'title_id': info['TITLE_ID'],
        'runner': 'rpcs3', 'platform': 'PlayStation 3', 'content': str(game),
        'icon': str(files[1]), 'background': str(files[2]), 'music': str(files[3])}
    (profile / 'library.json').write_text(json.dumps({'version': 1, 'games': [entry]}))
    config = root / 'config.json'
    config.write_text(json.dumps({'shell': {}, 'preferences': {'setup.completed': 'true',
        'startup.intro': 'Off', 'startup.warning': 'Off', 'audio.startup': 'Off',
        'artwork.steamgriddb.automatic': 'Off'}}))
    env = {key: value for key, value in os.environ.items() if not key.startswith(
        ('OPENXMB_SETUP_', 'OPENXMB_HARNESS_', 'OPENXMB_FIXED_', 'OPENXMB_INITIAL_', 'AUROREENGINE_HEADLESS'))}
    env.update({'OPENXMB_CONFIG': str(config), 'OPENXMB_DATA_DIR': str(root / 'data'),
        'XMB_ASSET_DIR': str(app.parent / 'shell'), 'XMB_LOCALE_DIR': str(app.parent / 'locales'),
        'OPENXMB_FORCE_SETUP': '0', 'SDL_AUDIODRIVER': 'dummy', 'SPDLOG_LEVEL': 'warn',
        'OPENXMB_INITIAL_CATEGORY': 'game', 'AUROREENGINE_HEADLESS': '1',
        'AUROREENGINE_HEADLESS_WIDTH': '960', 'AUROREENGINE_HEADLESS_HEIGHT': '540',
        'AUROREENGINE_HEADLESS_FRAMES': '96',
        'AUROREENGINE_HEADLESS_OUTPUT_PATTERN': 'frame-{:03d}.png'})
    frames = {}
    for name, actions in (('selected', ''), ('left-game', '0:right')):
        current = env | {'OPENXMB_HARNESS_ACTIONS': actions,
                         'AUROREENGINE_HEADLESS_OUTPUT_DIR': str(root / name)}
        result = subprocess.run([str(app)], cwd=app.parent, env=current, capture_output=True, timeout=90)
        (root / (name + '.log')).write_bytes(result.stdout + result.stderr)
        assert result.returncode == 0, (result.stdout + result.stderr).decode(errors='replace')
        path = sorted((root / name).glob('*.png'))[-1]
        frames[name] = Image.open(path).convert('RGB')
    selected, left = frames['selected'], frames['left-game']
    reference = Image.open(files[2]).convert('RGB').resize(selected.size, Image.Resampling.BILINEAR)
    points = [(int(selected.width * x), int(selected.height * y)) for x, y in
              ((.05, .8), (.6, .92), (.9, .9), (.95, .65))]
    for point in points:
        assert max(abs(a - b) for a, b in zip(selected.getpixel(point), reference.getpixel(point))) <= 8, (
            'PIC1 did not replace waves at an unobscured pixel', point,
            selected.getpixel(point), reference.getpixel(point))
    assert sum(max(abs(a - b) for a, b in zip(left.getpixel(point), reference.getpixel(point))) > 16
               for point in points) >= 2, 'Leaving the game did not restore the ordinary background'
    assert before == {path: hashlib.sha256(path.read_bytes()).digest() for path in files}, 'PS3 source media changed'
    print('PASS real PS3 presentation:', info['TITLE'], info['TITLE_ID'], 'PIC1 pixels, background restoration and source preservation')


if __name__ == '__main__':
    main()
