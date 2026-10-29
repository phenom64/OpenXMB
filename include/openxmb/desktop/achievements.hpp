#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openxmb::desktop {
enum class TrophyGrade { none, platinum, gold, silver, bronze };
struct Achievement {
  std::string id, title, description;
  TrophyGrade grade{TrophyGrade::none};
  std::optional<unsigned> points;
  std::filesystem::path icon;
  bool hidden{}, unlocked{};
  std::optional<bool> hardcore;
  bool unlock_state_known{};
  std::optional<std::int64_t> unlocked_at; // Unix seconds, if available
};
struct AchievementGame {
  std::string provider, user_id, game_id, title;
  std::filesystem::path icon, source_directory;
  std::vector<Achievement> entries;
  std::vector<std::string> warnings;
};
struct TrophyListResult {
  std::vector<std::string> values;
  std::string error;
  explicit operator bool() const noexcept { return error.empty(); }
};
struct TrophyReadResult {
  std::optional<AchievementGame> value;
  std::string error;
  explicit operator bool() const noexcept { return value.has_value() && error.empty(); }
};
// root is the explicitly selected RPCS3 dev_hdd0. No user profiles are merged.
[[nodiscard]] TrophyListResult rpcs3_trophy_users(const std::filesystem::path& root);
[[nodiscard]] TrophyListResult rpcs3_trophy_sets(const std::filesystem::path& root, const std::string& user_id);
[[nodiscard]] TrophyReadResult read_rpcs3_trophy_set(const std::filesystem::path& root, const std::string& user_id, const std::string& game_id);
} // namespace openxmb::desktop
