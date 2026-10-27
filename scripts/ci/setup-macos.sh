#!/usr/bin/env bash
set -euo pipefail
brew update --quiet
brew install llvm@20 cmake ninja pkg-config gettext ffmpeg fmt freetype harfbuzz \
  glm glslang libarchive zlib molten-vk sdl2 sdl2_image sdl2_mixer vulkan-loader poppler
{
  echo "$(brew --prefix llvm@20)/bin"
  echo "$(brew --prefix gettext)/bin"
} >> "$GITHUB_PATH"
{
  echo "CC=$(brew --prefix llvm@20)/bin/clang"
  echo "CXX=$(brew --prefix llvm@20)/bin/clang++"
  echo "CMAKE_PREFIX_PATH=$(brew --prefix):$(brew --prefix gettext):$(brew --prefix libarchive):$(brew --prefix zlib)"
  echo "PKG_CONFIG_PATH=$(brew --prefix)/lib/pkgconfig:$(brew --prefix gettext)/lib/pkgconfig"
  echo 'CMAKE_BUILD_PARALLEL_LEVEL=2'
  echo 'MACOSX_DEPLOYMENT_TARGET=15.0'
  echo 'SDL_AUDIODRIVER=dummy'
} >> "$GITHUB_ENV"
