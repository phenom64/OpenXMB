#!/usr/bin/env python3
"""Exercise rapid library artwork replacement with isolated data and Vulkan validation."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    from PIL import Image
    app, root = args.app.resolve(), args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    profile = root / 'data/profiles/default'
    profile.mkdir(parents=True)
    (root / 'data/profiles.json').write_text(json.dumps({'version': 1, 'active_profile': 'default',
        'profiles': [{'id': 'default', 'name': 'Artwork retirement fixture'}]}))
    games = []
    for index in range(16):
        icon = root / f'art-{index:02d}.png'
        Image.new('RGB', (256, 144), (30 + index * 12, 190 - index * 8, 50 + index * 7)).save(icon)
        games.append({'id': f'art-{index:02d}', 'title': f'Artwork fixture {index:02d}', 'icon': str(icon),
            # No runnable fixture is installed. Even an incorrect menu action
            # cannot start an executable or emulator.
            'executable': str(root / 'MISSING-DO-NOT-LAUNCH.exe')})
    (profile / 'library.json').write_text(json.dumps({'version': 1, 'games': games}))
    config = root / 'config.json'
    config.write_text(json.dumps({'shell': {}, 'preferences': {'setup.completed': 'true',
        'startup.intro': 'Off', 'startup.warning': 'Off', 'audio.startup': 'Off'}}))
    # Game entries precede the static Game Library entry. That panel's first
    # four rows are runner/add/folder/scan actions; the remaining rows own art.
    actions = ['0.1:down'] * len(games) + ['0.1:ok'] + ['0.6:down'] * 4
    actions += [f'{0.9 + index * .18:.2f}:down' for index in range(14)]
    actions += [f'{3.6 + index * .18:.2f}:up' for index in range(14)]
    env = {key: value for key, value in os.environ.items() if not key.startswith(
        ('OPENXMB_', 'AUROREENGINE_HEADLESS', 'RPCS3_'))}
    env.update({'OPENXMB_CONFIG': str(config), 'OPENXMB_CONFIG_READ_ONLY': '1',
        'OPENXMB_DATA_DIR': str(root / 'data'), 'OPENXMB_FORCE_SETUP': '0',
        'XMB_ASSET_DIR': str(app.parent / 'shell'), 'XMB_LOCALE_DIR': str(app.parent / 'locales'),
        'OPENXMB_INITIAL_CATEGORY': 'game', 'OPENXMB_HARNESS_ACTIONS': ';'.join(actions),
        'SDL_AUDIODRIVER': 'dummy', 'SPDLOG_LEVEL': 'debug', 'OPENXMB_VULKAN_VALIDATION': '1',
        'AUROREENGINE_HEADLESS': '1', 'AUROREENGINE_HEADLESS_WIDTH': '960',
        'AUROREENGINE_HEADLESS_HEIGHT': '540', 'AUROREENGINE_HEADLESS_FRAMES': '480',
        'AUROREENGINE_HEADLESS_OUTPUT_DIR': str(root / 'frames'),
        'AUROREENGINE_HEADLESS_OUTPUT_PATTERN': 'final.png'})
    result = subprocess.run([str(app)], cwd=app.parent, env=env, capture_output=True, timeout=120)
    log = (result.stdout + result.stderr).decode('utf-8', errors='replace')
    (root / 'native.log').write_text(log, encoding='utf-8')
    assert result.returncode == 0, log[-4000:]
    assert (root / 'frames/final.png').exists(), 'Native library frame missing'
    assert 'VUID-' not in log and 'Validation Error' not in log, log[-4000:]
    assert not (root / 'data/profiles/default/sessions/history.json').exists(), 'Fixture attempted to launch a game'
    loaded = {index for index in range(16) if f'art-{index:02d}.png' in log}
    assert len(loaded) >= 8, 'Rapid-selection fixture did not reach its artwork rows'
    validation_available = 'VK_LAYER_KHRONOS_validation is not available' not in log
    print('PASS rapid native library navigation, artwork replacement and frame output')
    print('Validation layer active' if validation_available else 'Validation layer unavailable; GPU lifetime validation remains pending')


if __name__ == '__main__':
    main()
