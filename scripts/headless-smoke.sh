#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT_DIR"

PRESET=${OPENXMB_CMAKE_PRESET:-ci}
if [ -n "${OPENXMB_BUILD_DIR:-}" ]; then
    BUILD_DIR=$OPENXMB_BUILD_DIR
else
    BUILD_DIR="$ROOT_DIR/build/$PRESET"
fi

if [ -n "${OPENXMB_INSTALL_PREFIX:-}" ]; then
    INSTALL_PREFIX=$OPENXMB_INSTALL_PREFIX
    CLEAN_INSTALL_PREFIX=false
else
    INSTALL_PREFIX="$BUILD_DIR/stage"
    CLEAN_INSTALL_PREFIX=true
fi

configure_with_preset=false
if [ "${OPENXMB_USE_PRESET:-1}" = "1" ]; then
    if cmake --list-presets >/dev/null 2>&1 && cmake --list-presets | grep -q "\"$PRESET\""; then
        configure_with_preset=true
    fi
fi

if [ "$configure_with_preset" = "true" ]; then
    cmake --preset "$PRESET"
else
    GENERATOR=${CMAKE_GENERATOR:-Ninja}
    BUILD_TYPE=${CMAKE_BUILD_TYPE:-Release}
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G "$GENERATOR" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DENABLE_BROWSER=OFF \
        -DENABLE_VIDEO_PLAYER=ON \
        -DENABLE_DISC_MEDIA=OFF \
        -DENABLE_LIBRETRO=OFF \
        -DINTERFACE_FX_DEBUG=OFF
fi

if [ "${OPENXMB_SMOKE_CONFIGURE_ONLY:-0}" = "1" ]; then
    echo "OpenXMB configure smoke passed: $BUILD_DIR"
    exit 0
fi

if [ "$configure_with_preset" = "true" ]; then
    cmake --build --preset "$PRESET"
else
    cmake --build "$BUILD_DIR"
fi

if [ "$CLEAN_INSTALL_PREFIX" = "true" ]; then
    rm -rf "$INSTALL_PREFIX"
fi
cmake --install "$BUILD_DIR" --prefix "$INSTALL_PREFIX"

require_path() {
    if [ ! -e "$1" ]; then
        echo "Missing required smoke artifact: $1" >&2
        exit 1
    fi
}

require_executable() {
    if [ ! -x "$1" ]; then
        echo "Missing required executable smoke artifact: $1" >&2
        exit 1
    fi
}

require_executable "$INSTALL_PREFIX/bin/XMS"
require_executable "$INSTALL_PREFIX/bin/XMS.bin"
require_path "$INSTALL_PREFIX/share/OpenXMB/config.json"
require_path "$INSTALL_PREFIX/share/shell/Play-Regular.ttf"
require_path "$INSTALL_PREFIX/share/shell/icons/icon_category_settings.png"
require_path "$INSTALL_PREFIX/share/shell/icons/icon_settings_background-type.png"
require_path "$INSTALL_PREFIX/share/shell/sounds/ok.wav"

if [ "${OPENXMB_RENDER_SMOKE:-0}" = "1" ]; then
    OPENXMB_INSTALL_PREFIX="$INSTALL_PREFIX" "$ROOT_DIR/scripts/headless-render-smoke.sh"
fi

echo "OpenXMB headless smoke passed: $INSTALL_PREFIX"
