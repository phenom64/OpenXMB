#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT_DIR"

WIDTH=${OPENXMB_RENDER_SMOKE_WIDTH:-640}
HEIGHT=${OPENXMB_RENDER_SMOKE_HEIGHT:-360}
WAIT_SECONDS=${OPENXMB_RENDER_SMOKE_WAIT_SECONDS:-12}
MIN_NONBLANK_RATIO=${OPENXMB_RENDER_SMOKE_MIN_NONBLANK_RATIO:-0.01}
MIN_YELLOW_PIXELS=${OPENXMB_RENDER_SMOKE_MIN_YELLOW_PIXELS:-20}
MAX_READY_SECONDS=${OPENXMB_RENDER_SMOKE_MAX_READY_SECONDS:-0}
KEEP_OUTPUT=${OPENXMB_RENDER_SMOKE_KEEP_OUTPUT:-0}
OUTPUT_DIR=${OPENXMB_RENDER_SMOKE_OUTPUT_DIR:-}
PRESET=${OPENXMB_CMAKE_PRESET:-ci}
BUILD_DIR=${OPENXMB_BUILD_DIR:-"$ROOT_DIR/build/$PRESET"}

if ! command -v python3 >/dev/null 2>&1; then
    echo "OpenXMB render smoke requires python3 for config and image checks." >&2
    exit 1
fi

if [ -n "${OPENXMB_INSTALL_PREFIX:-}" ]; then
    INSTALL_PREFIX=$OPENXMB_INSTALL_PREFIX
else
    INSTALL_PREFIX="$BUILD_DIR/stage"
fi

if [ -n "${OPENXMB_RENDER_SMOKE_APP:-}" ]; then
    APP=$OPENXMB_RENDER_SMOKE_APP
elif [ -x "$INSTALL_PREFIX/bin/XMS" ]; then
    APP="$INSTALL_PREFIX/bin/XMS"
elif [ -x "$INSTALL_PREFIX/bin/XMS.bin" ]; then
    APP="$INSTALL_PREFIX/bin/XMS.bin"
elif [ -x "$BUILD_DIR/XMS.sh" ]; then
    APP="$BUILD_DIR/XMS.sh"
else
    echo "Unable to find an OpenXMB executable for render smoke." >&2
    echo "Set OPENXMB_INSTALL_PREFIX, OPENXMB_BUILD_DIR, or OPENXMB_RENDER_SMOKE_APP." >&2
    exit 1
fi

UNAME=$(uname -s 2>/dev/null || echo unknown)
if [ "$UNAME" != "Darwin" ] && [ -z "${DISPLAY:-}" ] && [ -z "${OPENXMB_RENDER_SMOKE_UNDER_XVFB:-}" ]; then
    if command -v xvfb-run >/dev/null 2>&1; then
        exec xvfb-run -a -s "-screen 0 ${WIDTH}x${HEIGHT}x24" \
            env OPENXMB_RENDER_SMOKE_UNDER_XVFB=1 "$0" "$@"
    fi
    echo "No DISPLAY is set and xvfb-run was not found; cannot run headless render smoke." >&2
    exit 1
fi

if [ -n "$OUTPUT_DIR" ]; then
    RUN_DIR=$OUTPUT_DIR
    mkdir -p "$RUN_DIR"
    KEEP_OUTPUT=1
else
    RUN_DIR=$(mktemp -d "${TMPDIR:-/tmp}/openxmb-render-smoke.XXXXXX")
fi

APP_PID=
cleanup() {
    if [ -n "$APP_PID" ] && kill -0 "$APP_PID" >/dev/null 2>&1; then
        kill "$APP_PID" >/dev/null 2>&1 || true
        wait "$APP_PID" >/dev/null 2>&1 || true
    fi
    if [ "$KEEP_OUTPUT" != "1" ]; then
        rm -rf "$RUN_DIR"
    else
        echo "OpenXMB render smoke output kept at: $RUN_DIR"
    fi
}
trap cleanup EXIT INT TERM

CONFIG="$RUN_DIR/config.json"
cp "$ROOT_DIR/config.json" "$CONFIG"
python3 - "$CONFIG" <<'PY'
import json
import sys

path = sys.argv[1]
with open(path, "r", encoding="utf-8") as handle:
    config = json.load(handle)

shell = config.setdefault("shell", {})
render = config.setdefault("render", {})
shell["background-type"] = "original"
shell["theme-colour-mode"] = "custom"
shell["theme-custom-colour"] = "#3373f2"
shell["font-path"] = "default"
render["sample-count"] = 1
render["vsync"] = False
render["max-fps"] = 30
render["show-fps"] = False
render["show-mem"] = False

with open(path, "w", encoding="utf-8") as handle:
    json.dump(config, handle, indent=2)
    handle.write("\n")
PY

if [ -d "$INSTALL_PREFIX/share/shell" ]; then
    export XMB_ASSET_DIR=${XMB_ASSET_DIR:-"$INSTALL_PREFIX/share/shell"}
elif [ -d "$BUILD_DIR/share/shell" ]; then
    export XMB_ASSET_DIR=${XMB_ASSET_DIR:-"$BUILD_DIR/share/shell"}
fi

if [ -d "$INSTALL_PREFIX/share/locale" ]; then
    export XMB_LOCALE_DIR=${XMB_LOCALE_DIR:-"$INSTALL_PREFIX/share/locale"}
elif [ -d "$BUILD_DIR/locales" ]; then
    export XMB_LOCALE_DIR=${XMB_LOCALE_DIR:-"$BUILD_DIR/locales"}
fi

export OPENXMB_IFXDEBUG=1
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}

capture_frame() {
    out=$1
    backend_seen=0

    if [ "$UNAME" = "Darwin" ] && command -v screencapture >/dev/null 2>&1; then
        backend_seen=1
        screencapture -x "$out" >/dev/null 2>&1 && return 0
    fi

    if command -v import >/dev/null 2>&1; then
        backend_seen=1
        import -silent -window root "$out" >/dev/null 2>&1 && return 0
    fi

    if command -v xwd >/dev/null 2>&1; then
        backend_seen=1
        xwd_file="$RUN_DIR/capture.xwd"
        if xwd -root -silent -out "$xwd_file" >/dev/null 2>&1; then
            if command -v magick >/dev/null 2>&1; then
                magick "$xwd_file" "$out" >/dev/null 2>&1 && return 0
            elif command -v convert >/dev/null 2>&1; then
                convert "$xwd_file" "$out" >/dev/null 2>&1 && return 0
            fi
        fi
    fi

    if [ -n "${DISPLAY:-}" ] && command -v ffmpeg >/dev/null 2>&1; then
        backend_seen=1
        ffmpeg -loglevel error -y -f x11grab -video_size "${WIDTH}x${HEIGHT}" \
            -i "$DISPLAY" -frames:v 1 "$out" >/dev/null 2>&1 && return 0
    fi

    if [ "$backend_seen" -eq 0 ]; then
        return 2
    fi
    return 1
}

CHECKER="$ROOT_DIR/scripts/render-image-check.py"
LOG="$RUN_DIR/openxmb.log"
SHOT="$RUN_DIR/original-interfacefx.png"
CHECK_LOG="$RUN_DIR/image-check.log"
START_SECONDS=$(date +%s)
DEADLINE=$((START_SECONDS + WAIT_SECONDS))

(
    cd "$RUN_DIR"
    "$APP" --width "$WIDTH" --height "$HEIGHT" --no-fullscreen \
        --background-only --interfacefx-debug >"$LOG" 2>&1
) &
APP_PID=$!

CHECK_STATUS=1
CAPTURE_STATUS=1
READY_SECONDS=0
while [ "$(date +%s)" -le "$DEADLINE" ]; do
    if ! kill -0 "$APP_PID" >/dev/null 2>&1; then
        echo "OpenXMB exited before a render smoke frame could be captured." >&2
        sed -n '1,160p' "$LOG" >&2 || true
        exit 1
    fi

    if capture_frame "$SHOT"; then
        CAPTURE_STATUS=0
        if python3 "$CHECKER" "$SHOT" \
            --min-nonblank-ratio "$MIN_NONBLANK_RATIO" \
            --require-yellow \
            --min-yellow-pixels "$MIN_YELLOW_PIXELS" >"$CHECK_LOG" 2>&1; then
            CHECK_STATUS=0
            READY_SECONDS=$(( $(date +%s) - START_SECONDS ))
            break
        fi
    else
        CAPTURE_STATUS=$?
        if [ "$CAPTURE_STATUS" -eq 2 ]; then
            echo "No supported screenshot backend found." >&2
            echo "Install ImageMagick import, xwd+magick/convert, ffmpeg, or use macOS screencapture." >&2
            exit 1
        fi
    fi

    sleep 1
done

if [ "$CAPTURE_STATUS" -ne 0 ]; then
    echo "OpenXMB render smoke could not capture a frame within ${WAIT_SECONDS}s." >&2
    echo "A screenshot backend was present, but every capture attempt failed; on macOS this often means screen-capture permission is unavailable for the current shell." >&2
    sed -n '1,160p' "$LOG" >&2 || true
    exit 1
fi

if [ "$CHECK_STATUS" -ne 0 ]; then
    echo "OpenXMB render smoke captured a frame, but pixel checks failed." >&2
    sed -n '1,120p' "$CHECK_LOG" >&2 || true
    sed -n '1,160p' "$LOG" >&2 || true
    exit 1
fi

if [ "$MAX_READY_SECONDS" -gt 0 ] && [ "$READY_SECONDS" -gt "$MAX_READY_SECONDS" ]; then
    echo "OpenXMB render smoke exceeded ready-time budget: ${READY_SECONDS}s > ${MAX_READY_SECONDS}s" >&2
    exit 1
fi

cat "$CHECK_LOG"
echo "ready_seconds=$READY_SECONDS"
echo "OpenXMB render smoke passed: original background + InterfaceFX text probe"
