#!/usr/bin/env bash
set -euo pipefail
# Run only inside a disposable distro test container, not on a user's machine.
if [ "${OPENXMB_PORTABLE_CI:-}" != 1 ]; then
  echo 'This dependency bootstrap is restricted to disposable portability containers.' >&2
  exit 2
fi
distro=${1:?distro required}
archive=${2:?archive required}
output=${3:?mounted evidence directory required}
case "$distro" in
  ubuntu)
    export DEBIAN_FRONTEND=noninteractive
    apt-get -o Acquire::Retries=3 update
    apt-get -o Acquire::Retries=3 install -y --no-install-recommends python3 python3-pil ca-certificates mesa-vulkan-drivers libvulkan1 libegl-mesa0 fontconfig xvfb xauth imagemagick libxtst6 bubblewrap xdg-dbus-proxy dbus-x11 util-linux
    ;;
  fedora)
    dnf -y install python3 python3-pillow ca-certificates mesa-vulkan-drivers mesa-libEGL vulkan-loader fontconfig xorg-x11-server-Xvfb xorg-x11-xauth ImageMagick libXtst bubblewrap xdg-dbus-proxy dbus-daemon util-linux
    ;;
  arch)
    pacman -Syu --noconfirm --needed python python-pillow ca-certificates mesa vulkan-swrast vulkan-icd-loader fontconfig xorg-server-xvfb xorg-xauth imagemagick libxtst bubblewrap xdg-dbus-proxy dbus util-linux
    ;;
  *) echo "Unknown portability target: $distro" >&2; exit 2 ;;
esac
icd=$(find /usr/share/vulkan/icd.d -iname '*lvp*.json' -print -quit)
test -n "$icd"
export VK_ICD_FILENAMES="$icd"
export SDL_AUDIODRIVER=dummy
export WEBKIT_DISABLE_COMPOSITING_MODE=1
cat /etc/os-release
getconf GNU_LIBC_VERSION
for tool in setpriv dbus-run-session xvfb-run Xvfb xauth import bwrap xdg-dbus-proxy; do
  command -v "$tool" >/dev/null
done
test -d "$output"
chmod 777 "$output"
work=$(mktemp -d '/tmp/openxmb portable user ü.XXXXXX')
chown 65534:65534 "$work"
setpriv --reuid 65534 --regid 65534 --clear-groups env \
  HOME="$work" TMPDIR="$work" XDG_RUNTIME_DIR="$work" PYTHONDONTWRITEBYTECODE=1 \
  dbus-run-session -- python3 - "$archive" "$output" <<'PY'
import os
from pathlib import Path
import subprocess
import sys

archive, output = sys.argv[1:]
sys.path.insert(0, '/src/scripts/packaging')
import portable_smoke

run_extracted = portable_smoke.run_extracted

def test_archive(app, smoke_output, version, render):
    run_extracted(app, smoke_output, version, render)
    subprocess.run([sys.executable, '/src/tests/integrations/web_view_smoke.py',
                    '--helper', str(app / 'bin/openxmb-web'),
                    '--output', str(Path(output) / 'browser'), '--https-check',
                    '--require-sandbox', '--persistence-check'],
                   # xvfb-run's Xauthority setup cannot handle a spaced TMPDIR.
                   # Keep the extracted application path's spaces and Unicode;
                   # use a plain directory only for the display's auth files.
                   env=dict(os.environ, TMPDIR='/tmp'), check=True)

portable_smoke.run_extracted = test_archive
sys.argv = ['portable_smoke.py', '--archive', archive, '--output', str(Path(output) / 'portable')]
portable_smoke.main()
PY
