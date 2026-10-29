#!/usr/bin/env python3
"""Render binding-aware setup instructions and reference-directed digit motion."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    from PIL import Image, ImageChops
    app, output = args.app.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    images = {}
    with tempfile.TemporaryDirectory(prefix='openxmb-setup-controls-') as temporary:
        root = Path(temporary)
        scenarios = [('keyboard', 'keyboard', {}, '', None), ('playstation', 'playstation', {}, '', None),
            ('xbox', 'xbox', {}, '', None), ('remapped', 'keyboard', {'key.up': '26'}, '', None),
            ('increase-start', 'playstation', {}, '0:up', '0'),
            ('increase-middle', 'playstation', {}, '0:up', '0.04'),
            ('increase-end', 'playstation', {}, '0:up', '0.08'),
            ('decrease-middle', 'playstation', {}, '0:down', '0.04')]
        for name, family, preferences, actions, age in scenarios:
            scene = output / name
            scene.mkdir(parents=True, exist_ok=True)
            config = root / 'config.json'
            config.write_text(json.dumps({'shell': {'font-path': 'default'}, 'controller': {'type': family},
                'preferences': {'setup.completed': 'false'} | preferences}))
            env = {key: value for key, value in os.environ.items() if not key.startswith(('OPENXMB_', 'AUROREENGINE_HEADLESS'))}
            env.update({'OPENXMB_CONFIG': str(config), 'OPENXMB_DATA_DIR': str(root / 'data'),
                'OPENXMB_CONFIG_READ_ONLY': '1', 'OPENXMB_FORCE_SETUP': '1',
                'OPENXMB_SETUP_PAGE': 'date-time', 'OPENXMB_SETUP_EDIT': '1',
                'OPENXMB_FIXED_UNIX_SECONDS': '1767225600', 'OPENXMB_HARNESS_ACTIONS': actions,
                'XMB_ASSET_DIR': str(app.parent / 'shell'), 'XMB_LOCALE_DIR': str(app.parent / 'locales'),
                'SDL_AUDIODRIVER': 'dummy', 'SPDLOG_LEVEL': 'warn', 'AUROREENGINE_HEADLESS': '1',
                'AUROREENGINE_HEADLESS_WIDTH': '1920', 'AUROREENGINE_HEADLESS_HEIGHT': '1080',
                'AUROREENGINE_HEADLESS_FRAMES': '4', 'AUROREENGINE_HEADLESS_OUTPUT_DIR': str(scene),
                'AUROREENGINE_HEADLESS_OUTPUT_PATTERN': 'frame-{:03d}.png'})
            if age is not None:
                env['OPENXMB_FIXED_VALUE_SECONDS'] = age
            result = subprocess.run([str(app)], cwd=app.parent, env=env, capture_output=True,
                encoding='utf-8', errors='replace', timeout=60)
            (scene / 'native.log').write_text(result.stdout + result.stderr, encoding='utf-8')
            assert result.returncode == 0, result.stdout + result.stderr
            with Image.open(sorted(scene.glob('frame-*.png'))[-1]) as frame:
                images[name] = frame.convert('RGB')
        instruction = (70, 815, 1850, 875)
        for other in ('playstation', 'xbox', 'remapped'):
            assert ImageChops.difference(images['keyboard'].crop(instruction), images[other].crop(instruction)).getbbox(), \
                f'Inline controls did not reflect {other} bindings'
        # This column misses the stationary spinner mark. Incoming increment
        # ink must appear above the stable cap band, not merely change its glow.
        above = (838, 519, 848, 527)
        peak = lambda name, box: images[name].crop(box).getextrema()[0][1]
        assert peak('increase-middle', above) > 150, 'Increment did not glide in from above'
        assert peak('increase-start', above) < 150 and peak('increase-end', above) < 150, \
            'Digit motion did not start/end at the stationary baseline'
        below = (815, 552, 823, 559)
        assert peak('decrease-middle', below) > 150, 'Wrapped decrement did not glide in from below'
    print('PASS native inline keyboard/PlayStation/Xbox/remapped glyphs and up/down fading digit motion')


if __name__ == '__main__':
    main()
