#!/usr/bin/env python3
"""Launch a real child from a native Game entry using an isolated profile."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--child', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    app, child = args.app.resolve(), args.child.resolve()
    with tempfile.TemporaryDirectory(prefix='openxmb-game-') as temporary:
        root = Path(temporary)
        profile = root / 'data/profiles/default'
        profile.mkdir(parents=True)
        cwd = root / 'game with spaces 日本'
        cwd.mkdir()
        sentinel = cwd / 'unrelated.txt'
        sentinel.write_text('Keep game files.', encoding='utf-8')
        (root / 'data/profiles.json').write_text(json.dumps({
            'version': 1, 'active_profile': 'default',
            'profiles': [{'id': 'default', 'name': 'Fixture'}],
        }), encoding='utf-8')
        config = root / 'config.json'
        config.write_text(json.dumps({'shell': {}, 'preferences': {
            'setup.completed': 'true', 'games.launch': 'Keep Running',
        }}), encoding='utf-8')
        literal = 'literal ; $(touch injected) & 日本'
        game = {'id': 'native-fixture', 'title': 'Native Game Fixture',
                'description': 'Real direct launch from the Game crossbar',
                'executable': str(child), 'working_directory': str(cwd),
                'arguments': ['record', '7', '80', literal]}
        library = profile / 'library.json'
        library.write_text(json.dumps({'version': 1, 'games': [game]}), encoding='utf-8')
        env = os.environ.copy()
        for key in list(env):
            if key.startswith(('OPENXMB_SETUP_', 'OPENXMB_HARNESS_', 'OPENXMB_FIXED_',
                               'OPENXMB_INITIAL_', 'AUROREENGINE_HEADLESS_')):
                env.pop(key)
        env.update({
            'OPENXMB_CONFIG': str(config), 'OPENXMB_DATA_DIR': str(root / 'data'),
            'XMB_ASSET_DIR': str(app.parent / 'shell'), 'XMB_LOCALE_DIR': str(app.parent / 'locales'),
            'OPENXMB_FORCE_SETUP': '0', 'OPENXMB_INITIAL_CATEGORY': 'game',
            'OPENXMB_HARNESS_ACTIONS': '0:ok', 'SDL_AUDIODRIVER': 'dummy',
            'AUROREENGINE_HEADLESS': '1', 'AUROREENGINE_HEADLESS_WIDTH': '1280',
            'AUROREENGINE_HEADLESS_HEIGHT': '720', 'AUROREENGINE_HEADLESS_FRAMES': '32',
            'SPDLOG_LEVEL': 'warn',
        })
        if args.output:
            args.output.resolve().mkdir(parents=True, exist_ok=True)
            env['AUROREENGINE_HEADLESS_OUTPUT_DIR'] = str(args.output.resolve())
            env['AUROREENGINE_HEADLESS_OUTPUT_PATTERN'] = 'frame-{:03d}.png'
        result = subprocess.run([str(app)], cwd=app.parent, env=env, capture_output=True,
                                encoding='utf-8', errors='replace', timeout=90)
        assert result.returncode == 0, result.stdout + result.stderr
        assert (cwd / 'arguments.json').exists(), 'Native Game entry did not launch the child'
        received = json.loads((cwd / 'arguments.json').read_text(encoding='utf-8'))
        assert received['arguments'][1:] == game['arguments'], received
        assert Path(received['cwd']).samefile(cwd), received
        journal = json.loads((profile / 'sessions/history.json').read_text(encoding='utf-8'))
        records = journal['recent']
        assert len(records) == 1 and records[0]['identity'] == game['id'], journal
        assert records[0]['state'] == 1 and records[0]['exit_code'] == 7, journal
        assert records[0]['duration_ms'] >= 70, journal
        assert journal['totals'][0]['launch_count'] == 1, journal
        assert json.loads(library.read_text(encoding='utf-8'))['games'][0]['play_count'] == 1
        assert sentinel.read_text(encoding='utf-8') == 'Keep game files.'
        assert not (cwd / 'injected').exists()
    print('Native crossbar launch, literal argv, exit, profile history and file preservation passed')


if __name__ == '__main__':
    main()
