#!/usr/bin/env bash
set -euo pipefail
if [[ "${OPENXMB_HOTKEY_TEST_INNER:-}" != 1 ]]; then
  exec xvfb-run -a env OPENXMB_HOTKEY_TEST_INNER=1 bash "$0"
fi
cd "$(dirname "$0")/../.."
out="${OPENXMB_X11_TEST_OUTPUT:-$HOME/.cache/openxmb-x11-tools}"
mkdir -p "$out"
extra=()
if [[ -d "$out/extracted/usr/include" ]]; then extra+=("-I$out/extracted/usr/include" -I/usr/include/xcb); fi
clang++ -std=c++23 -Wall -Wextra -Werror -Iinclude "${extra[@]}" \
  tests/integrations/session_hotkey_tests.cpp src/integrations/session_hotkey.cpp \
  -lxcb -lX11 -ldl -pthread -o "$out/session-hotkey-tests"
unset WAYLAND_DISPLAY
export XDG_SESSION_TYPE=x11 OPENXMB_HOTKEY_TEST_ISOLATED=1
"$out/session-hotkey-tests"
