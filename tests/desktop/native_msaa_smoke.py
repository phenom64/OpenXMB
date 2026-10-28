#!/usr/bin/env python3
"""Exercise native X11 live MSAA 4 -> 1 -> 4 with an isolated profile.

Requires the existing desktop-smoke dependencies, a Vulkan validation layer,
and a device supporting 4x MSAA. --output must name a new directory.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]


def validation_log_issue(log):
    """Return a diagnostic unless the engine callback was active and error-free."""
    if re.search(r'validation requested, but VK_LAYER_KHRONOS_validation is not available', log, re.I):
        return 'the validation layer was unavailable'
    initialized = re.search(r'Initialized Vulkan with (\d+) layer\(s\)', log)
    if not initialized or int(initialized.group(1)) < 1:
        return 'the engine did not report an enabled validation layer'
    if not re.search(r'\[Vulkan Validation\]', log):
        return 'no message from the engine validation callback was observed'
    if re.search(r'VUID-|\[(?:error|critical)\]\s*\[Vulkan Validation\]|'
                 r'Unhandled [^\n]*exception|Segmentation fault|access violation|'
                 r'terminate called', log, re.I):
        return 'a Vulkan validation error or crash was reported'
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--check-log', type=Path, help='check an existing engine log without running the app')
    parser.add_argument('--expect-invalid', action='store_true', help='expect --check-log to be rejected')
    args = parser.parse_args()
    if args.check_log:
        issue = validation_log_issue(args.check_log.read_text(errors='replace'))
        print(f'{args.check_log}: {issue or "validation active; no errors detected"}')
        if bool(issue) != args.expect_invalid:
            raise SystemExit(1)
        return
    if args.expect_invalid or not args.app or not args.output:
        parser.error('--app and --output are required for a native run; --expect-invalid requires --check-log')
    if sys.platform != 'linux':
        parser.error('this native-input harness requires Linux/X11')
    if not os.environ.get('DISPLAY'):
        os.execvp('xvfb-run', ['xvfb-run', '-a', '-s', '-screen 0 1920x1080x24',
                              sys.executable, __file__, *sys.argv[1:]])
    spec = importlib.util.spec_from_file_location('desktop_smoke', ROOT / 'scripts/desktop-smoke.py')
    smoke = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = smoke
    spec.loader.exec_module(smoke)
    options = argparse.Namespace(app=args.app.resolve(), assets=None, output=args.output,
        cases='', timeout=90, width=1920, height=1080, no_ocr=True, require_ocr=False,
        keep_going=False, native_window=True, under_xvfb=True, generate_only=False)
    runner = smoke.Runner(options)
    profile = runner.output / 'profile'
    profile.mkdir()
    config = json.loads((ROOT / 'config.json').read_text())
    config.setdefault('preferences', {}).update({'startup.intro': 'Off', 'startup.warning': 'Off'})
    config.setdefault('render', {}).update({'sample-count': 4, 'max-fps': 30,
        'vsync': False, 'show-fps': False, 'show-mem': False})
    config.setdefault('shell', {}).update({'background-type': 'original', 'font-path': 'default'})
    smoke.json_write(profile / 'config.json', config)
    env = {'OPENXMB_INITIAL_CATEGORY': 'settings',
           'OPENXMB_INITIAL_SETTINGS_MENU': 'category.settings.desktop.Display',
           'OPENXMB_INITIAL_SETTINGS_SELECTION': '7',
           'OPENXMB_VULKAN_VALIDATION': '1',
           'VK_INSTANCE_LAYERS': 'VK_LAYER_KHRONOS_validation'}
    with smoke.Session(runner, 'live-msaa', [], reuse=profile, environment_patch=env) as session:
        settings = session.wait(lambda image: smoke.foreground(image, (0.2, 0.2, 0.9, 0.85)) > 0.002,
                                'display-settings', delay=3)
        session.log_handle.flush()
        issue = validation_log_issue(session.log.read_text(errors='replace'))
        if issue:
            raise smoke.SmokeFailure(f'{issue}; see {session.log}')
        session.key('enter')
        session.wait(lambda image: smoke.foreground(image, (0.2, 0.2, 0.9, 0.85)) > 0.002 and
                     smoke.difference(settings, image, (0.2, 0.2, 0.95, 0.85)) > 0.002,
                     'choices-at-4', delay=1)
        session.key('up', 2)
        session.key('enter')
        session.wait_file(session.configuration, lambda data: data['render']['sample-count'] == 1)
        settings_at_one = session.wait(
            lambda image: smoke.foreground(image, (0.2, 0.2, 0.9, 0.85)) > 0.002,
            'rendering-at-1', delay=3)
        session.key('enter')
        session.wait(lambda image: smoke.foreground(image, (0.2, 0.2, 0.9, 0.85)) > 0.002 and
                     smoke.difference(settings_at_one, image, (0.2, 0.2, 0.95, 0.85)) > 0.002,
                     'choices-at-1', delay=1)
        session.key('down', 2)
        session.key('enter')
        session.wait_file(session.configuration, lambda data: data['render']['sample-count'] == 4)
        session.wait(lambda image: smoke.foreground(image, (0.2, 0.2, 0.9, 0.85)) > 0.002,
                     'rendering-again-at-4', delay=3)
    log = session.log.read_text(errors='replace')
    issue = validation_log_issue(log)
    if issue:
        raise smoke.SmokeFailure(f'{issue}; see {session.log}')
    print(f'Live 4 -> 1 -> 4 completed; inspect {session.log} and captured frames')


if __name__ == '__main__':
    main()
