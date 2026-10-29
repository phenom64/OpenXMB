#!/usr/bin/env python3
"""Verify setup confirmation compositing through the native Vulkan renderer."""
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
    from PIL import Image
    app, output = args.app.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    levels = []
    with tempfile.TemporaryDirectory(prefix='openxmb-setup-frame-') as temporary:
        root = Path(temporary)
        config = root / 'config.json'
        config.write_text(json.dumps({'shell': {}, 'preferences': {'setup.completed': 'false'}}))
        for name, elapsed in (('start', '0'), ('middle', '0.125'), ('settled', '1.2')):
            scene = output / name
            scene.mkdir(parents=True, exist_ok=True)
            env = {k: v for k, v in os.environ.items() if not k.startswith(('OPENXMB_', 'AUROREENGINE_HEADLESS'))}
            env.update({'OPENXMB_CONFIG': str(config), 'OPENXMB_DATA_DIR': str(root / 'data'),
                'OPENXMB_CONFIG_READ_ONLY': '1', 'OPENXMB_FORCE_SETUP': '1',
                'OPENXMB_SETUP_PAGE': 'confirmation', 'OPENXMB_FIXED_SETUP_SECONDS': elapsed,
                'XMB_ASSET_DIR': str(app.parent / 'shell'), 'XMB_LOCALE_DIR': str(app.parent / 'locales'),
                'SDL_AUDIODRIVER': 'dummy', 'SPDLOG_LEVEL': 'warn', 'AUROREENGINE_HEADLESS': '1',
                'AUROREENGINE_HEADLESS_WIDTH': '1920', 'AUROREENGINE_HEADLESS_HEIGHT': '1080',
                'AUROREENGINE_HEADLESS_FRAMES': '4', 'AUROREENGINE_HEADLESS_OUTPUT_DIR': str(scene),
                'AUROREENGINE_HEADLESS_OUTPUT_PATTERN': 'frame-{:03d}.png'})
            result = subprocess.run([str(app)], cwd=app.parent, env=env, capture_output=True,
                encoding='utf-8', errors='replace', timeout=60)
            (scene / 'native.log').write_text(result.stdout + result.stderr, encoding='utf-8')
            assert result.returncode == 0, result.stdout + result.stderr
            with Image.open(sorted(scene.glob('frame-*.png'))[-1]) as frame:
                frame = frame.convert('RGB')
                background = frame.getpixel((50, 500))[0]
                levels.append(background)
                assert frame.crop((160, 165, 315, 195)).getextrema()[0][1] > 220, 'Confirmation dimmed the header'
                assert frame.crop((700, 895, 1300, 925)).getextrema()[0][1] > 220, 'Confirmation dimmed the footer'
                if name == 'settled':
                    assert 8 <= background <= 12, ('Full-frame reference lift is missing', background)
                    body_peak = frame.crop((800, 300, 1120, 330)).getextrema()[0][1]
                    assert 20 <= body_peak <= 40, ('Underlying setup page was not shaded', body_peak)
                    assert frame.crop((348, 455, 650, 480)).getextrema()[0][1] > 220, 'Confirmation question is not bright'
        assert levels[0] <= 1 and levels[0] < levels[1] < levels[2], ('Fade did not advance with the pinned clock', levels)
    print('PASS native setup confirmation: timed full-frame shade, subdued page, bright dialog/header/footer')


if __name__ == '__main__':
    main()
