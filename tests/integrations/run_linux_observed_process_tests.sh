#!/usr/bin/env bash
# Linux native X11 provider-observer tests. Always a NEW private Xvfb server
# (never the person's desktop). Nothing here signals a process: fixtures end on
# a control file, WM_DELETE_WINDOW, or their own timeout.
#   OPENXMB_OBSERVED_SOURCE  observed_process.cpp to test (default: the product file;
#                            point at a __linux__-neutralised copy for a red run)
set -euo pipefail
if [[ "${OPENXMB_OBSERVED_TEST_INNER:-}" != 1 ]]; then
  exec xvfb-run -a -s "-screen 0 1024x768x24 -nolisten tcp" \
    env OPENXMB_OBSERVED_TEST_INNER=1 OPENXMB_OBSERVED_OUTER_DISPLAY="${DISPLAY:-}" bash "$0"
fi
cd "$(dirname "$0")/../.."
if [[ -n "${OPENXMB_OBSERVED_OUTER_DISPLAY:-}" && "$DISPLAY" == "$OPENXMB_OBSERVED_OUTER_DISPLAY" ]]; then
  echo "Refusing to run against the outer display $DISPLAY" >&2; exit 2
fi
out="${OPENXMB_X11_TEST_OUTPUT:-$HOME/.cache/openxmb-x11-tools}"
mkdir -p "$out"
# Product code is a read-only identity capability: it must not manage processes.
if grep -nE '\b(kill|killpg|tgkill|pthread_kill|raise|SIGTERM|SIGKILL|SIGINT|setpgid|setpgrp|setsid)\b' \
    src/integrations/observed_process.cpp src/integrations/linux_observed_process.hpp; then
  echo "Observer must not signal or manage processes" >&2; exit 3
fi
extra=()
if [[ -d "$out/extracted/usr/include" ]]; then extra+=("-I$out/extracted/usr/include" -I/usr/include/xcb); fi
source_file="${OPENXMB_OBSERVED_SOURCE:-src/integrations/observed_process.cpp}"
binary="${OPENXMB_OBSERVED_TEST_BINARY:-}"
if [[ -z "$binary" ]]; then
  binary="$out/linux-observed-tests"
  clang++ -std=c++23 -Wall -Wextra -Werror -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -Iinclude -Isrc/integrations "${extra[@]}" \
    tests/integrations/linux_observed_process_tests.cpp "$source_file" src/integrations/session_window.cpp \
    -lX11 -lxcb -l:libxcb-res.so.0 -ldl -pthread -o "$binary"
fi
export OPENXMB_X11_TEST_ISOLATED=1
unset WAYLAND_DISPLAY
export XDG_SESSION_TYPE=x11
"$binary"
