"""The render smoke must run against its own profile and never touch a personal one."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'scripts/headless-render-smoke.sh'
SH = shutil.which('sh')

# Fake child. Records what it received, then behaves like the unfixed launch: when
# it is not given a run-owned config/data root it falls back to the "personal" profile
# named by PERSONAL_PROFILE and writes there, as main.cpp would restore/create it.
FAKE_APP = r'''#!/bin/sh
{
  printf 'config=%s\n' "${OPENXMB_CONFIG-<unset>}"
  printf 'read_only=%s\n' "${OPENXMB_CONFIG_READ_ONLY-<unset>}"
  printf 'data=%s\n' "${OPENXMB_DATA_DIR-<unset>}"
  printf 'ifxdebug=%s\n' "${OPENXMB_IFXDEBUG-<unset>}"
  printf 'args=%s\n' "$*"
  printf 'cwd=%s\n' "$(pwd)"
  for name in AUROREENGINE_HEADLESS OPENXMB_FORCE_SETUP OPENXMB_SETUP_PAGE OPENXMB_HEADLESS_STARTUP OPENXMB_HARNESS_ACTIONS OPENXMB_INITIAL_CATEGORY; do
    eval "value=\${$name-<unset>}"
    printf '%s=%s\n' "$name" "$value"
  done
} > "$RECORD"
if [ -n "${OPENXMB_CONFIG:-}" ]; then
  cat "$OPENXMB_CONFIG" > "$RECORD.config" 2>"$RECORD.err"
fi
if [ -z "${OPENXMB_CONFIG:-}" ] || [ "${OPENXMB_CONFIG_READ_ONLY:-}" != 1 ]; then
  printf 'restored-profile\n' >> "$PERSONAL_PROFILE"
fi
if [ -z "${OPENXMB_DATA_DIR:-}" ] || [ "$OPENXMB_DATA_DIR" = "$PERSONAL_DATA" ]; then
  printf 'leaked\n' >> "$PERSONAL_DATA/profiles.json"
fi
: > "$RECORD.done"
exit 0
'''


@unittest.skipIf(SH is None, 'POSIX sh required')
class HeadlessSmokeIsolationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        base = Path(self.temp.name)
        self.personal_profile = base / 'personal' / 'config.json'
        self.personal_data = base / 'personal' / 'data'
        self.personal_data.mkdir(parents=True)
        self.personal_profile.write_bytes(b'{"sentinel":"personal profile"}\n')
        (self.personal_data / 'profiles.json').write_bytes(b'{"active":"personal"}\n')
        self.sentinels = {path: (path.read_bytes(), path.stat().st_mtime_ns)
                          for path in (self.personal_profile, self.personal_data / 'profiles.json')}
        self.app = base / 'fake-xms'
        self.app.write_text(FAKE_APP, encoding='utf-8', newline='\n')
        self.app.chmod(0o755)
        self.output = base / 'run'
        self.record = base / 'record.txt'

    def run_smoke(self, extra_env=None):
        env = {k: v for k, v in os.environ.items() if not k.startswith('OPENXMB_')}
        env.update({
            'DISPLAY': ':0', 'OPENXMB_RENDER_SMOKE_APP': str(self.app),
            'OPENXMB_RENDER_SMOKE_OUTPUT_DIR': str(self.output),
            'OPENXMB_RENDER_SMOKE_WAIT_SECONDS': '2',
            'RECORD': str(self.record), 'PERSONAL_PROFILE': str(self.personal_profile),
            'PERSONAL_DATA': str(self.personal_data),
            # Hostile inherited state that must not reach the child.
            'AUROREENGINE_HEADLESS': '1',
            'OPENXMB_CONFIG': str(self.personal_profile),
            'OPENXMB_CONFIG_READ_ONLY': '0',
            'OPENXMB_DATA_DIR': str(self.personal_data),
            'OPENXMB_FORCE_SETUP': '1', 'OPENXMB_SETUP_PAGE': 'brand',
            'OPENXMB_HEADLESS_STARTUP': '1', 'OPENXMB_HARNESS_ACTIONS': '1:ok',
            'OPENXMB_INITIAL_CATEGORY': 'games',
        })
        env.update(extra_env or {})
        return subprocess.run([SH, str(SCRIPT)], cwd=ROOT, env=env, text=True,
                              capture_output=True, timeout=60)

    def child_record(self):
        return dict(line.split('=', 1) for line in self.record.read_text().splitlines())

    def test_child_gets_run_owned_readonly_profile_and_personal_files_are_untouched(self):
        result = self.run_smoke()
        # The fake exits before any frame exists (or no capture backend is installed),
        # so the smoke itself must fail; only the child's view is under test.
        self.assertEqual(result.returncode, 1, result.stderr)
        # The smoke does not wait for its child; wait for the fake to finish.
        done = Path(str(self.record) + '.done')
        deadline = time.monotonic() + 20
        while not done.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertTrue(done.exists(), 'fake child did not finish')

        child = self.child_record()
        run_config = self.output / 'config.json'
        self.assertEqual(Path(child['config']).resolve(), run_config.resolve())
        self.assertEqual(child['read_only'], '1')
        self.assertEqual(Path(child['data']).resolve(), (self.output / 'data').resolve())
        for name in ('AUROREENGINE_HEADLESS', 'OPENXMB_FORCE_SETUP', 'OPENXMB_SETUP_PAGE', 'OPENXMB_HEADLESS_STARTUP',
                     'OPENXMB_HARNESS_ACTIONS', 'OPENXMB_INITIAL_CATEGORY'):
            self.assertEqual(child[name], '<unset>', name)
        self.assertEqual(child['ifxdebug'], '1')
        # Intended rendering mode is preserved.
        self.assertIn('--background-only', child['args'])
        self.assertIn('--no-fullscreen', child['args'])
        self.assertIn('--interfacefx-debug', child['args'])

        generated = json.loads(Path(str(self.record) + '.config').read_text())
        self.assertEqual(generated['shell']['background-type'], 'original')
        self.assertEqual(generated['render']['sample-count'], 1)
        self.assertEqual(generated['preferences']['setup.completed'], 'true')

        for path, (content, mtime) in self.sentinels.items():
            self.assertEqual(path.read_bytes(), content, path)
            self.assertEqual(path.stat().st_mtime_ns, mtime, path)


if __name__ == '__main__':
    unittest.main()
