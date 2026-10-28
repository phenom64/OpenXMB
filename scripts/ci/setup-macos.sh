#!/usr/bin/env bash
set -euo pipefail
if [ "${GITHUB_ACTIONS:-}" != 'true' ]; then
  echo 'This script resets dependencies on disposable GitHub-hosted runners only.' >&2
  exit 1
fi
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_ANALYTICS=1
# Keep both architectures on one bottle set, including Intel binaries that are
# no longer provided by the current rolling Homebrew catalogue.
core_snapshot=8d030826c130a1e71eb4db25cad2ac4ace581b74
brew_snapshot=aa278895809b4c079e560fb4fb9bf17e48cf65f1
brew_repository="$(brew --repository)"
installed=()
while IFS= read -r formula; do
  [ -n "$formula" ] && installed+=("$formula")
done < <(brew list --formula)
if [ "${#installed[@]}" -gt 0 ]; then
  brew uninstall --ignore-dependencies --force "${installed[@]}"
fi
/usr/bin/git -C "$brew_repository" fetch --depth 1 https://github.com/Homebrew/brew.git "$brew_snapshot"
/usr/bin/git -C "$brew_repository" checkout --force --detach FETCH_HEAD
core_repository="$brew_repository/Library/Taps/homebrew/homebrew-core"
mkdir -p "$core_repository"
/usr/bin/git -C "$core_repository" init
/usr/bin/git -C "$core_repository" fetch --depth 1 https://github.com/Homebrew/homebrew-core.git "$core_snapshot"
/usr/bin/git -C "$core_repository" checkout --force --detach FETCH_HEAD
export HOMEBREW_NO_INSTALL_FROM_API=1
brew install --force-bottle llvm@20 cmake ninja pkg-config gettext ffmpeg fmt freetype harfbuzz \
  glm glslang libarchive libxmp zlib molten-vk sdl2 sdl2_image sdl2_mixer vulkan-loader poppler
{
  echo "$(brew --prefix llvm@20)/bin"
  echo "$(brew --prefix gettext)/bin"
} >> "$GITHUB_PATH"
{
  echo "CC=$(brew --prefix llvm@20)/bin/clang"
  echo "CXX=$(brew --prefix llvm@20)/bin/clang++"
  echo "CMAKE_PREFIX_PATH=$(brew --prefix):$(brew --prefix gettext):$(brew --prefix libarchive):$(brew --prefix zlib)"
  echo "PKG_CONFIG_PATH=$(brew --prefix)/lib/pkgconfig:$(brew --prefix gettext)/lib/pkgconfig"
  echo 'MACOSX_DEPLOYMENT_TARGET=15.0'
  echo 'SDL_AUDIODRIVER=dummy'
} >> "$GITHUB_ENV"
