#pragma once
#include "openxmb/desktop/achievements.hpp"
#include "openxmb/desktop/http.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
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
// Official rcheevos console IDs (include/rc_consoles.h) with a verified
// whole-file MD5 recipe (src/rhash/hash.c: rc_hash_whole_file). Nothing else is
// hashed; manual game IDs remain the route for every other format.
enum class RetroRomFormat : unsigned { game_boy=4, game_boy_advance=5, game_boy_color=6 };
inline constexpr std::size_t retroachievements_match_limit=64;
using RetroCancel=std::function<bool()>;
enum class RetroRomHashStatus { ok, unsupported, invalid, unreadable, changed, cancelled };
struct RetroRomHash { std::string md5; RetroRomFormat format{}; std::uintmax_t size{}; };
struct RetroRomHashResult {
  std::optional<RetroRomHash> value; RetroRomHashStatus status{RetroRomHashStatus::unreadable}; std::string error;
  explicit operator bool() const noexcept { return value.has_value() && status==RetroRomHashStatus::ok; }
};
// Local, read-only, chunked MD5 of a raw .gb/.gbc/.gba file (lowercase hex of
// the whole file, as rcheevos). Rejects other extensions, non-regular files and
// out-of-range sizes, polls `cancelled` per chunk and fails if the file's size,
// modification time or canonical path changed while it was read.
[[nodiscard]] RetroRomHashResult retroachievements_hash_rom(const std::filesystem::path& path, const RetroCancel& cancelled={});
enum class RetroMatchStatus { matched, unmatched, unsupported, unavailable, skipped };
struct RetroMatchItem {
  std::filesystem::path path; RetroMatchStatus status{RetroMatchStatus::unavailable};
  unsigned game_id{}; std::string message; bool stale{};
};
struct RetroMatchResult {
  std::vector<RetroMatchItem> items;
  unsigned matched{}, unmatched{}, unsupported{}, unavailable{}, skipped{};
  std::string error, warning;
  bool cancelled{}, rate_limited{}, from_cache{}, stale{};
};
// Hashes each path locally (at most retroachievements_match_limit; extras are
// `skipped`) and looks the hash up in RetroAchievements' official per-console
// hash list (API_GetGameList.php?h=1&f=1). Only a hash listed for exactly one
// game is `matched`; absent or ambiguous hashes are `unmatched`. Titles are
// never used. The list is cached per account; a list younger than max_age is
// reused without a request, and a stale list is used (and flagged) when
// refreshing is not possible. 429 stops further requests in the call.
// Only the hash list request leaves the machine, and it carries the key,
// never ROM bytes or paths. The key is transient.
[[nodiscard]] RetroMatchResult retroachievements_match(const std::filesystem::path& cache_root, const RetroAccount& account,
  const std::string& key, const std::vector<std::filesystem::path>& paths, const HttpTransport& transport,
  const RetroCancel& cancelled={}, bool refresh=true, std::uint64_t max_age_seconds=86400);
} // namespace openxmb::desktop
