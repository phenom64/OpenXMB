#!/usr/bin/env python3
"""Exercise rapid setup locale changes without rebuilding resident fonts/menus."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--max-switch-ms', type=float, default=50)
    args = parser.parse_args()
    app, output = args.app.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='openxmb-language-') as temporary:
        root = Path(temporary)
        config = root / 'config.json'
        config.write_text(json.dumps({'shell': {'language': 'en', 'font-path': 'default'},
            'preferences': {'setup.completed': 'false'}}))
        env = {key: value for key, value in os.environ.items() if not key.startswith(('OPENXMB_', 'AUROREENGINE_HEADLESS'))}
        env.update({'OPENXMB_CONFIG': str(config), 'OPENXMB_DATA_DIR': str(root / 'data'),
            'OPENXMB_CONFIG_READ_ONLY': '1', 'OPENXMB_FORCE_SETUP': '1',
            'OPENXMB_SETUP_PAGE': 'language', 'OPENXMB_HARNESS_ACTIONS':
                '0.2:down;0.4:down;0.6:down;0.8:up;1.0:up;1.2:up;1.4:quit',
            'XMB_ASSET_DIR': str(app.parent / 'shell'), 'XMB_LOCALE_DIR': str(app.parent / 'locales'),
            'SDL_AUDIODRIVER': 'dummy', 'SPDLOG_LEVEL': 'debug', 'AUROREENGINE_HEADLESS': '1',
            'AUROREENGINE_HEADLESS_WIDTH': '1280', 'AUROREENGINE_HEADLESS_HEIGHT': '720',
            'AUROREENGINE_HEADLESS_FRAMES': '100000'})
        result = subprocess.run([str(app)], cwd=app.parent, env=env, capture_output=True,
            encoding='utf-8', errors='replace', timeout=60)
        log = result.stdout + result.stderr
        (output / 'native.log').write_text(log, encoding='utf-8')
        assert result.returncode == 0, log
        preparations = re.findall(r"Preparing shell font '(.*?)'; setup=(\w+)", log)
        assert len(preparations) == 1 and preparations[0][1] == 'true', ('Setup rebuilt its font', preparations)
        assert Path(preparations[0][0]).name == 'Play-Regular.ttf', ('Original theme lost Play', preparations)
        switches = re.findall(r"Language '([^']+)' applied in ([\d.]+) ms; setup=true, font resources retained", log)
        assert [language for language, _ in switches] == ['de', 'fr', 'hi', 'fr', 'de', 'en'], switches
        durations = [float(duration) for _, duration in switches]
        assert max(durations) < args.max_switch_ms, ('Setup language switch exceeded latency budget', durations)
        tail = log[log.index("Language 'de' applied"):]
        assert 'Loaded catalog-backed Settings controller' not in tail, 'Setup rebuilt hidden menus'
        print(f'PASS six live setup language switches: max {max(durations):.3f} ms; one resident Play/fallback font, no menu rebuild')


if __name__ == '__main__':
    main()
