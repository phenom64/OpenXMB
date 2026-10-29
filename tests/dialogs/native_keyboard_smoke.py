#!/usr/bin/env python3
"""Native OSK press/release feedback and actual local-completion acceptance."""
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
    with tempfile.TemporaryDirectory(prefix='openxmb-osk-') as temporary:
        root = Path(temporary)

        def run(name, actions, complete=False, preferences=None):
            scene = output / name
            scene.mkdir(parents=True, exist_ok=True)
            config = root / (name + '.json')
            config.write_text(json.dumps({'shell': {'font-path': 'default', 'language': 'en'},
                'controller': {'type': 'keyboard'}, 'preferences': {'setup.completed': 'false'} | (preferences or {})}))
            env = {key: value for key, value in os.environ.items() if not key.startswith(('OPENXMB_', 'AUROREENGINE_HEADLESS'))}
            env.update({'OPENXMB_CONFIG': str(config), 'OPENXMB_DATA_DIR': str(root / name),
                'OPENXMB_CONFIG_READ_ONLY': '0' if complete else '1', 'OPENXMB_FORCE_SETUP': '1',
                'OPENXMB_SETUP_PAGE': 'name', 'OPENXMB_HARNESS_ACTIONS': actions, 'USERNAME': 'mu',
                'XMB_ASSET_DIR': str(app.parent / 'shell'), 'XMB_LOCALE_DIR': str(app.parent / 'locales'),
                'SDL_AUDIODRIVER': 'dummy', 'SPDLOG_LEVEL': 'trace', 'AUROREENGINE_HEADLESS': '1',
                'AUROREENGINE_HEADLESS_WIDTH': '1920', 'AUROREENGINE_HEADLESS_HEIGHT': '1080',
                'AUROREENGINE_HEADLESS_FRAMES': '100000' if complete else '30',
                'AUROREENGINE_HEADLESS_OUTPUT_DIR': str(scene),
                'AUROREENGINE_HEADLESS_OUTPUT_PATTERN': 'frame-{:03d}.png'})
            result = subprocess.run([str(app)], cwd=app.parent, env=env, capture_output=True,
                encoding='utf-8', errors='replace', timeout=60)
            (scene / 'native.log').write_text(result.stdout + result.stderr, encoding='utf-8')
            (scene / 'saved-config.json').write_text(config.read_text(encoding='utf-8'), encoding='utf-8')
            assert result.returncode == 0, result.stdout + result.stderr
            with Image.open(sorted(scene.glob('frame-*.png'))[-1]) as frame:
                rendered = frame.convert('RGB')
            return rendered, json.loads(config.read_text(encoding='utf-8'))

        resting, _ = run('resting', '0:ok')
        pressed, _ = run('physical-a-held', '0:ok;0.05:key-down=4')
        released, _ = run('physical-a-released', '0:ok;0.05:key-down=4;0.15:key-up=4')
        # Inside A's flat face, away from its legend and the G focus frame.
        point = (526, 537)
        normal, down, up = [frame.getpixel(point)[0] for frame in (resting, pressed, released)]
        assert normal > down + 8 and abs(normal - up) <= 3, ('Physical key feedback did not press/release', normal, down, up)
        # Let the keyboard's 140 ms dismissal and underlying 250 ms transition
        # finish before navigating the next screen; input during closing is ignored.
        actions = '0:ok;' + ';'.join(['0.35:right'] * 6) + \
            ';0.6:ok;0.8:key-down=40;0.81:key-up=40;1.4:right;1.8:ok;2.2:left;2.6:ok;3.2:quit'
        _, saved = run('completion-accepted', actions, complete=True)
        assert saved['preferences']['setup.completed'] == 'true', 'Native completion flow did not finish setup'
        assert saved['preferences']['user.name'] == 'multi', ('Completion did not replace the prefix', saved['preferences']['user.name'])
        # Exercise the actual shell SDL-axis route. Repeated pressure changes
        # above the release threshold must not toggle either shortcut again.
        actions = ('0:ok;0.35:axis=4,23000;0.5:axis=4,18000;0.6:axis=4,12000;'
            '0.75:axis=4,0;0.85:axis=4,23000;1:axis=4,0;'
            '1.1:axis=5,23000;1.2:axis=5,19000;1.3:axis=5,14000;1.4:axis=5,0;'
            '1.5:ok;1.8:start;2.3:right;2.7:ok;3.1:left;3.5:ok;4.1:quit')
        _, saved = run('controller-triggers', actions, complete=True)
        assert saved['preferences']['setup.completed'] == 'true', 'Start did not submit the controller keyboard'
        assert saved['preferences']['user.name'] == 'muG', ('Trigger press/release changed the wrong page or case', saved['preferences']['user.name'])
        actions = ('0:ok;0.35:axis=4,23000;0.5:axis=4,19000;0.9:axis=4,0;'
            '1.2:start;1.7:right;2.1:ok;2.5:left;2.9:ok;3.5:quit')
        _, saved = run('controller-trigger-remap', actions, complete=True, preferences={'pad.ok': '21'})
        assert saved['preferences']['setup.completed'] == 'true', 'Remapped trigger flow did not finish setup'
        assert saved['preferences']['user.name'] == 'mug', ('L2 Confirm was ignored or fell through to accents', saved['preferences']['user.name'])
    print('PASS native keyboard: physical press/release, real completion and save, SDL L2/R2 hysteresis, Start submission')


if __name__ == '__main__':
    main()
