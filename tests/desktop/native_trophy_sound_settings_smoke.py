#!/usr/bin/env python3
"""Check the real trophy-audio Settings action using private files, no unlocks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import wave


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    app, root = args.app.resolve(), args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    for name in ('valid', 'invalid', 'missing', 'off'):
        case = root / name
        data = case / 'data'
        profile = data / 'profiles/default'
        profile.mkdir(parents=True)
        (data / 'profiles.json').write_text(json.dumps({'version': 1, 'active_profile': 'default',
            'profiles': [{'id': 'default', 'name': 'Sound settings fixture'}]}))
        audio = case / 'User fixture café.wav'
        if name in ('valid', 'off'):
            with wave.open(str(audio), 'wb') as out:
                out.setnchannels(1)
                out.setsampwidth(2)
                out.setframerate(22050)
                # An explicitly generated fixture; never packaged as a PS3 sound.
                out.writeframes(b''.join(struct.pack('<h', 2000 if i % 20 < 10 else -2000)
                                         for i in range(4410)))
        elif name == 'invalid':
            audio.write_bytes(b'Invalid file fixture, not audio.')
        before = (hashlib.sha256(audio.read_bytes()).digest(), audio.stat().st_mtime_ns) if audio.exists() else None
        sentinel = case / 'unrelated.bin'
        sentinel.write_bytes(b'Unrelated user files must survive audio settings.')
        sentinel_before = sentinel.read_bytes()
        config = case / 'config.json'
        config.write_text(json.dumps({'shell': {}, 'preferences': {
            'setup.completed': 'true', 'startup.intro': 'Off', 'startup.warning': 'Off',
            'audio.startup': 'Off', 'audio.trophy.mode': 'User-supplied' if name == 'off' else 'Off',
            'audio.trophy.file': str(audio),
        }}))
        env = {key: value for key, value in os.environ.items() if not key.startswith((
            'OPENXMB_', 'AUROREENGINE_HEADLESS', 'RPCS3_'))}
        env.update({'OPENXMB_CONFIG': str(config), 'OPENXMB_DATA_DIR': str(data),
            'XMB_ASSET_DIR': str(app.parent / 'shell'), 'XMB_LOCALE_DIR': str(app.parent / 'locales'),
            'OPENXMB_FORCE_SETUP': '0', 'SDL_AUDIODRIVER': 'dummy', 'SPDLOG_LEVEL': 'debug',
            'OPENXMB_INITIAL_CATEGORY': 'settings',
            'OPENXMB_INITIAL_SETTINGS_MENU': 'category.settings.desktop.Sound',
            'OPENXMB_INITIAL_SETTINGS_SELECTION': '3',
            'OPENXMB_HARNESS_ACTIONS': '0.2:ok;0.4:' + ('up' if name == 'off' else 'down') + ';0.6:ok',
            'AUROREENGINE_HEADLESS': '1', 'AUROREENGINE_HEADLESS_WIDTH': '1280',
            'AUROREENGINE_HEADLESS_HEIGHT': '720', 'AUROREENGINE_HEADLESS_FRAMES': '100',
            'AUROREENGINE_HEADLESS_OUTPUT_DIR': str(case / 'frames'),
            'AUROREENGINE_HEADLESS_OUTPUT_PATTERN': 'final.png'})
        result = subprocess.run([str(app)], cwd=app.parent, env=env, capture_output=True, timeout=90)
        log = (result.stdout + result.stderr).decode(errors='replace')
        (case / 'native.log').write_text(log, encoding='utf-8')
        assert result.returncode == 0, log
        saved = json.loads(config.read_text(encoding='utf-8-sig'))['preferences']
        expected = 'User-supplied' if name == 'valid' else 'Off'
        assert saved['audio.trophy.mode'] == expected, (name, saved['audio.trophy.mode'], expected)
        assert saved['audio.trophy.file'] == str(audio), 'Settings replaced the user-selected file'
        after = (hashlib.sha256(audio.read_bytes()).digest(), audio.stat().st_mtime_ns) if audio.exists() else None
        assert after == before, 'Settings changed, removed or created the user audio file'
        assert sentinel.read_bytes() == sentinel_before, 'Settings changed an unrelated file'
        assert (case / 'frames/final.png').is_file(), 'Native settings produced no frame'
        print(f'PASS native trophy sound settings: {name}; profile value and external files preserved')


if __name__ == '__main__':
    main()
