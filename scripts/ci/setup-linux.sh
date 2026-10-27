#!/usr/bin/env bash
set -euo pipefail
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  clang-19 clang-tools-19 lld-19 g++-14 cmake ninja-build pkg-config \
  gettext libarchive-dev zlib1g-dev libx11-dev libxtst-dev libcurl4-openssl-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev \
  libfmt-dev libfreetype-dev libglm-dev libharfbuzz-dev \
  libsdl2-dev libsdl2-image-dev libsdl2-mixer-dev libvulkan-dev \
  glslang-tools spirv-tools vulkan-utility-libraries-dev mesa-vulkan-drivers \
  libgtk-3-dev libwebkit2gtk-4.1-dev bubblewrap xdg-dbus-proxy apparmor \
  ffmpeg poppler-utils xvfb xauth imagemagick dpkg-dev file patchelf
# Allow only the CI helper to create WebKit's private sandbox namespaces.
# Keep the host's AppArmor policy and WebKit's bubblewrap/seccomp sandbox active.
if [ "${GITHUB_ACTIONS:-}" = 'true' ] && sudo aa-enabled --quiet; then
  printf 'abi <abi/4.0>,\nprofile openxmb-ci-web "%s/**/openxmb-web" flags=(unconfined) {\n  userns,\n}\n' "$PWD" \
    | sudo tee /etc/apparmor.d/openxmb-ci-web >/dev/null
  sudo apparmor_parser -r /etc/apparmor.d/openxmb-ci-web
fi
{
  echo 'CC=clang-19'
  echo 'CXX=clang++-19'
  echo 'CMAKE_LINKER_TYPE=LLD'
  echo 'CMAKE_BUILD_PARALLEL_LEVEL=2'
  echo 'SDL_AUDIODRIVER=dummy'
} >> "$GITHUB_ENV"
