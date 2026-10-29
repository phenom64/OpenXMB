#!/usr/bin/env bash
set -euo pipefail
if [[ "${OPENXMB_X11_TEST_INNER:-}" != 1 ]]; then
  # Never redirect events, focus or a compositor to a person's desktop.
  exec xvfb-run -a env OPENXMB_X11_TEST_INNER=1 bash "$0"
fi
cd "$(dirname "$0")/../.."
out="${OPENXMB_X11_TEST_OUTPUT:-$HOME/.cache/openxmb-x11-tools}"
mkdir -p "$out"
# Optional extracted dev headers for isolated WSL validation. Normal systems
# install libxcb1-dev, libxcb-render0-dev, libxcb-res0-dev and libpng-dev.
extra=()
if [[ -d "$out/extracted/usr/include" ]]; then extra+=("-I$out/extracted/usr/include" -I/usr/include/xcb); fi
test_binary="${OPENXMB_X11_TEST_BINARY:-}"
if [[ -z "$test_binary" ]]; then
  clang++ -std=c++23 -Wall -Wextra -Werror -Iinclude "${extra[@]}" \
    tests/integrations/x11_session_tests.cpp src/integrations/xmb_overlay.cpp \
    src/integrations/session_window.cpp src/integrations/owned_process.cpp \
    -lX11 -lxcb -l:libxcb-render.so.0 -l:libxcb-res.so.0 -ldl -pthread -o "$out/x11-tests"
  test_binary="$out/x11-tests"
fi
export OPENXMB_X11_TEST_ISOLATED=1
unset WAYLAND_DISPLAY
export XDG_SESSION_TYPE=x11
"$test_binary" --no-compositor
compositor="${OPENXMB_X11_COMPOSITOR:-}"
if [[ -z "$compositor" ]]; then compositor="$(command -v xcompmgr || true)"; fi
if [[ -z "$compositor" ]]; then compositor="$out/extracted/usr/bin/xcompmgr"; fi
"$compositor" -n >"$out/compositor.log" 2>&1 &
compositor_pid=$!
trap 'kill "$compositor_pid" 2>/dev/null || true' EXIT
sleep 0.2
"$test_binary"
