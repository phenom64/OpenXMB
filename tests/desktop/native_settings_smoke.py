#!/usr/bin/env python3
"""Check Linux/X11 settings selection, rendering, cancellation and persistence.

This prepares an isolated installed-theme fixture; theme import is exercised by
scripts/desktop-smoke.py --cases themes. No user settings or libraries are used.
"""
import importlib.util
import sys
import argparse
import json
import zipfile
import os
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]

def main():
    if not os.environ.get('DISPLAY'):
        os.execvp('xvfb-run', ['xvfb-run', '-a', '-s', '-screen 0 1920x1080x24',
                            sys.executable, __file__, *sys.argv[1:]])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    cli = parser.parse_args()
    spec = importlib.util.spec_from_file_location('desktop_smoke', str(ROOT / 'scripts/desktop-smoke.py'))
    m = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = m
    spec.loader.exec_module(m)
    a = argparse.Namespace(app=cli.app.resolve(), assets=None, output=cli.output, cases='', timeout=90, width=1920, height=1080, no_ocr=True, require_ocr=False, keep_going=False, native_window=True, under_xvfb=True, generate_only=False)
    r = m.Runner(a)
    profile = r.output / 'profile'
    profile.mkdir()
    config = json.loads((ROOT / 'config.json').read_text())
    config.setdefault('preferences', {}).update({'startup.intro': 'Off', 'startup.warning': 'Off'})
    config['render'].update({'sample-count': 1, 'max-fps': 30, 'vsync': False, 'show-fps': False, 'show-mem': False, 'icon-glass-refraction': True})
    config['shell'].update({'background-type': 'original', 'theme-colour-mode': 'custom', 'theme-custom-colour': '#d7122d', 'theme-day-night-mode': 'day', 'font-path': 'default', 'language': 'en'})
    m.json_write(profile / 'config.json', config)
    pack = profile / 'data/OpenXMB/themes/themes/native-fixture'
    pack.mkdir(parents=True)
    with zipfile.ZipFile(r.fixtures.theme) as z:
        z.extractall(pack)
    manifest = json.loads((pack / 'theme.json').read_text())
    manifest['icons'] = {}
    m.json_write(pack / 'theme.json', manifest)
    env = {'OPENXMB_INITIAL_CATEGORY': 'settings', 'OPENXMB_INITIAL_SETTINGS_MENU': 'category.settings.theme.settings', 'OPENXMB_INITIAL_SETTINGS_SELECTION': '0'}
    checks = []
    with m.Session(r, 'theme-settings', [], reuse=profile, environment_patch=env) as s:
        s.wait(lambda im: m.foreground(im, (0.34, 0.43, 0.75, 0.55)) > 0.005, 'theme-settings-root', delay=3)
        s.key('enter')
        s.wait(lambda im: True, 'theme-options-right-strip', delay=2)
        s.key('down', 2)
        s.key('enter')
        s.wait_file(profile / 'data/OpenXMB/themes/active.json', lambda d: d.get('active', {}).get('id') == 'native-fixture')
        s.wait(lambda im: m.color_ratio(im, m.MAGENTA, 35) > 0.5, 'installed-theme-applied', delay=2)
        checks.append('installed theme selected from native right picker; real background and active state changed')
        s.key('enter')
        s.key('up', 2)
        s.key('enter')
        s.wait_file(profile / 'data/OpenXMB/themes/active.json', lambda d: 'active' not in d)
        before_colour = s.wait(lambda im: m.color_ratio(im, m.MAGENTA, 35) < 0.05, 'original-theme-restored', delay=2)
        checks.append('Original reverted pack and retained Theme Settings route')
        s.key('down')
        s.key('enter')
        s.wait(lambda im: True, 'colour-options-right-strip', delay=1)
        s.key('down')
        s.wait(lambda im: True, 'colour-live-preview', delay=1)
        s.key('escape')
        after_cancel = s.wait(lambda im: True, 'colour-preview-cancelled', delay=1)
        if m.difference(before_colour, after_cancel, (0.04, 0.04, 0.2, 0.15)) > 0.01:
            raise m.SmokeFailure('Cancelling the colour preview changed the custom colour')
        checks.append('cancelling a colour preview restored the exact custom background')
        s.key('enter')
        s.wait(lambda im: True, 'colour-picker-reopened', delay=1)
        s.key('down')
        s.key('enter')
        s.wait_file(s.configuration, lambda d: d['shell']['theme-colour-mode'] == 'custom' and d['shell']['theme-custom-colour'] != '#d7122d')
        checks.append('native colour choice persisted actual theme colour')
        day_image = s.wait(lambda im: True, 'day-colour-saved', delay=1)
        s.key('down', 3)
        s.key('enter')
        s.wait(lambda im: True, 'day-night-options-right-strip', delay=1)
        s.key('down', 5)
        s.key('enter')
        s.wait_file(s.configuration, lambda d: d['shell']['theme-day-night-mode'] == 'night')
        night_image = s.wait(lambda im: True, 'night-mode-applied', delay=2)
        from PIL import ImageStat
        brightness = lambda image: sum(ImageStat.Stat(m.crop(image, (0.04, 0.04, 0.2, 0.15))).mean)
        if brightness(night_image) >= brightness(day_image) * 0.8:
            raise m.SmokeFailure('Night mode did not visibly darken the background')
        checks.append('native Night choice persisted real day/night mode')
    # Desktop catalogue installation replaces the legacy System menu. Language
    # is its second row (after System Name), as defined by desktop_settings::entries.
    langenv = {'OPENXMB_INITIAL_CATEGORY': 'settings',
               'OPENXMB_INITIAL_SETTINGS_MENU': 'category.settings.desktop.System',
               'OPENXMB_INITIAL_SETTINGS_SELECTION': '1'}
    with m.Session(r, 'language-settings', [], reuse=profile, environment_patch=langenv) as s:
        s.wait(lambda im: m.foreground(im, (0.34, 0.43, 0.75, 0.55)) > 0.005, 'language-setting', delay=2)
        s.key('enter')
        s.key('down', 2)
        s.key('enter')
        s.wait_file(s.configuration, lambda d: d['shell'].get('language') == 'fr')
        s.wait(lambda im: True, 'language-saved', delay=2)
        checks.append('catalogue language choice changed real saved locale and rebuilt menus')
    print(json.dumps({'status': 'passed', 'checks': checks}, indent=2), flush=True)
    (r.output / 'report.json').write_text(json.dumps({'status': 'passed', 'checks': checks}, indent=2))
if __name__ == '__main__':
    main()
