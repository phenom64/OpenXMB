#pragma once

#include "openxmb/integrations/desktop.hpp"

namespace openxmb::integrations {

enum class DiscKind { absent, audio_cd, dvd_video, blu_ray, data };
struct DiscInfo {
  DiscKind kind{DiscKind::absent};
  std::filesystem::path path;
  std::string label;
  // An existing media stream can also be opened by OpenXMB's native player.
  std::filesystem::path representative_stream;
};
[[nodiscard]] DiscInfo probe_disc(const std::filesystem::path& path);
[[nodiscard]] std::vector<DiscInfo> discover_discs(
    const std::vector<std::filesystem::path>& roots = {});
[[nodiscard]] std::string_view disc_kind_name(DiscKind kind) noexcept;
[[nodiscard]] ProcessResult play_disc(const DiscInfo& disc);

} // namespace openxmb::integrations
