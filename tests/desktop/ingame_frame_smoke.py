#!/usr/bin/env python3
"""No-focus native Vulkan test of the in-game XMB frame's per-pixel alpha."""
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
    with tempfile.TemporaryDirectory(prefix='openxmb-ingame-frame-') as temp:
        root = Path(temp)
        config = root / 'config.json'
        config.write_text(json.dumps({'shell': {}, 'preferences': {'setup.completed': 'true',
            'startup.intro': 'Off', 'startup.warning': 'Off', 'audio.startup': 'Off'}}))
        env = {k: v for k, v in os.environ.items() if not k.startswith(('OPENXMB_', 'AUROREENGINE_HEADLESS'))}
        env.update({'OPENXMB_CONFIG': str(config), 'OPENXMB_DATA_DIR': str(root / 'data'),
            'XMB_ASSET_DIR': str(app.parent / 'shell'), 'XMB_LOCALE_DIR': str(app.parent / 'locales'),
            'OPENXMB_FORCE_SETUP': '0', 'OPENXMB_INITIAL_CATEGORY': 'game',
            'OPENXMB_HARNESS_ACTIONS': '0:in-game', 'SDL_AUDIODRIVER': 'dummy', 'SPDLOG_LEVEL': 'warn',
            'AUROREENGINE_HEADLESS': '1', 'AUROREENGINE_HEADLESS_WIDTH': '1280',
            'AUROREENGINE_HEADLESS_HEIGHT': '720', 'AUROREENGINE_HEADLESS_FRAMES': '16',
            'AUROREENGINE_HEADLESS_OUTPUT_DIR': str(output),
            'AUROREENGINE_HEADLESS_OUTPUT_PATTERN': 'frame-{:03d}.png'})
        result = subprocess.run([str(app)], cwd=app.parent, env=env, capture_output=True,
            encoding='utf-8', errors='replace', timeout=60)
        (output / 'native.log').write_text(result.stdout + result.stderr, encoding='utf-8')
        assert result.returncode == 0, result.stdout + result.stderr
    frames = sorted(output.glob('frame-*.png'))
    assert frames, 'No GPU frames captured'
    with Image.open(frames[-1]) as frame:
        assert frame.mode == 'RGBA', frame.mode
        assert frame.size == (1280, 720), frame.size
        for point in ((80, 600), (1100, 600), (640, 710)):
            r, g, b, alpha = frame.getpixel(point)
            assert (r, g, b) == (0, 0, 0) and 126 <= alpha <= 129, (point, (r, g, b, alpha))
        assert frame.getchannel('A').getextrema()[1] > 240, 'Text/icons lost their independent opacity'
    print('PASS native GPU in-game frame: half-transparent black background, independently opaque XMB content')


if __name__ == '__main__':
    main()
