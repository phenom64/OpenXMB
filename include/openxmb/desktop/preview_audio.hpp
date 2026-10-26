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
