/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025-2026 Syndromatic Ltd. All rights reserved
 * Designed by Kavish Krishnakumar in Manchester.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

namespace openxmb::desktop {
// A bounded preview channel, separate from the persistent music transport.
// FFmpeg decoding runs on one cancellable worker; SDL_mixer owns output timing.
class PreviewAudio {
public:
  PreviewAudio();
  ~PreviewAudio();
  PreviewAudio(const PreviewAudio &) = delete;
  PreviewAudio &operator=(const PreviewAudio &) = delete;
  void set_path(const std::filesystem::path &path, float volume = 0.5f);
  void tick();
  void stop();
  bool active() const noexcept;
  bool loading() const noexcept;
  std::size_t decoded_bytes() const noexcept;
  std::string error() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace openxmb::desktop
