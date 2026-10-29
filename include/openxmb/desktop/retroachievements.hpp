#pragma once
#include "openxmb/desktop/achievements.hpp"
#include "openxmb/desktop/http.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
namespace openxmb::desktop {
struct RetroAccount { std::string username, ulid; };
struct RetroAccountResult {
  std::optional<RetroAccount> value; std::string error; bool rate_limited{};
  explicit operator bool() const noexcept { return value.has_value() && error.empty(); }
};
struct RetroProgressDetail {
  std::string id, badge_url;
  std::optional<std::int64_t> softcore_at, hardcore_at;
};
struct RetroProgressResult {
  std::optional<AchievementGame> game;
  std::vector<RetroProgressDetail> details;
  std::string game_icon_url, error, warning;
  bool from_cache{}, stale{}, rate_limited{};
  std::optional<std::int64_t> fetched_at;
  explicit operator bool() const noexcept { return game.has_value() && error.empty(); }
};
// Key is transient. Do not log the URL or retain it in settings or cache.
[[nodiscard]] RetroAccountResult retroachievements_account(const std::string& username, const std::string& key, const HttpTransport& transport);
// Explicit game ID only. refresh=false reads cache without contacting the API.
// 429 never triggers an automatic retry; callers decide when to retry.
[[nodiscard]] RetroProgressResult retroachievements_progress(const std::filesystem::path& cache_root, const RetroAccount& account,
  unsigned game_id, const std::string& key, const HttpTransport& transport, bool refresh=true, std::uint64_t max_age_seconds=86400);
} // namespace openxmb::desktop
