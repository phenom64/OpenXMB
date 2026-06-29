#pragma once

#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace openxmb::xmb {

[[nodiscard]] inline std::optional<std::string>
normal_map_number_from_icon_filename(std::string_view filename) noexcept {
  constexpr std::string_view xmb_prefix = "xmb_icon_";
  constexpr std::string_view icontex_prefix = "icontex_";
  constexpr std::string_view suffix = ".png";

  if (filename.size() == xmb_prefix.size() + 3 + suffix.size() &&
      filename.rfind(xmb_prefix, 0) == 0 &&
      filename.substr(filename.size() - suffix.size()) == suffix) {
    const auto number = filename.substr(xmb_prefix.size(), 3);
    for (const auto character : number) {
      if (!std::isdigit(static_cast<unsigned char>(character))) {
        return std::nullopt;
      }
    }
    return std::string{number};
  }

  if (filename.size() >= icontex_prefix.size() + suffix.size() &&
      filename.rfind(icontex_prefix, 0) == 0 &&
      filename.substr(filename.size() - suffix.size()) == suffix) {
    const auto stem =
        filename.substr(icontex_prefix.size(),
                        filename.size() - icontex_prefix.size() - suffix.size());
    const auto underscore = stem.find('_');
    const auto number = underscore == std::string_view::npos ? stem : stem.substr(0, underscore);
    if (number.size() != 3) {
      return std::nullopt;
    }
    for (const auto character : number) {
      if (!std::isdigit(static_cast<unsigned char>(character))) {
        return std::nullopt;
      }
    }
    return std::string{number};
  }

  return std::nullopt;
}

[[nodiscard]] inline std::optional<std::filesystem::path>
xmb_web_normal_map_for_icon(const std::filesystem::path &icon_path) {
  if (const auto number =
          normal_map_number_from_icon_filename(icon_path.filename().string())) {
    const auto normal_map =
        icon_path.parent_path().parent_path() / "normalmaps" /
        ("nmap_" + *number + ".png");
    std::error_code error;
    if (std::filesystem::exists(normal_map, error) && !error) {
      return normal_map;
    }
  }
  return std::nullopt;
}

} // namespace openxmb::xmb