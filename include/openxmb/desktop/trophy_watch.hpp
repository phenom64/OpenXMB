#pragma once
#include "openxmb/desktop/achievements.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace openxmb::desktop {
struct TrophyWatchSession {
  std::uint64_t generation{};
  std::string session_id;
  std::filesystem::path profile_directory,game_directory,rpcs3_hdd0;
  std::string rpcs3_user_id;
  bool operator==(const TrophyWatchSession&) const = default;
};
struct TrophyUnlockEvent {
  TrophyWatchSession session;
  std::string provider,user_id,set_id,trophy_id,game_title;
  std::filesystem::path source_directory;
  // Opaque native directory/file identity: path reuse cannot impersonate the
  // source that produced this edge. The event owns all of its display data.
  std::string source_identity;
  Achievement achievement;
};
struct TrophyWatchStatus {
  bool watching{},baseline_ready{};
  std::size_t associated_sets{},queued{},dropped{};
  std::uint64_t completed_polls{};
  std::string error;
};
// No credentials, source writes, emulator interaction or UI callbacks. Only
// exact TROPDIR set IDs belonging to the active game and selected RPCS3 user
// are observed. The first stable known state is always a silent baseline.
class Rpcs3TrophyWatch {
public:
  explicit Rpcs3TrophyWatch(std::chrono::milliseconds poll_interval=std::chrono::milliseconds(250));
  ~Rpcs3TrophyWatch();
  Rpcs3TrophyWatch(const Rpcs3TrophyWatch&)=delete;
  Rpcs3TrophyWatch& operator=(const Rpcs3TrophyWatch&)=delete;
  // Value-only and nonjoining. Change generation for every session/profile
  // incarnation; nullopt cancels immediately and discards queued old events.
  void set_session(std::optional<TrophyWatchSession> session);
  [[nodiscard]] std::vector<TrophyUnlockEvent> take_events();
  [[nodiscard]] TrophyWatchStatus status() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace openxmb::desktop
