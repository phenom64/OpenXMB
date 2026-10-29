#pragma once

#include "openxmb/integrations/desktop.hpp"
#include "openxmb/integrations/session_window.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace openxmb::integrations { struct SessionWindowTarget; }

namespace openxmb::desktop {
enum class SessionKind { game, application };
enum class TrackingQuality { direct_child, launcher_only, unknown, provider_observed };
enum class SessionState { running, exited, detached, interrupted };
struct ProviderObservationSpec {
  std::filesystem::path executable;
  std::uint32_t timeout_ms{60000};
};
struct SessionCapabilities {bool focus{}, capture{}, graceful_close{};};
struct SessionDescriptor {
  std::string identity, title, provider, runner;
  std::filesystem::path profile_directory;
  SessionKind kind{SessionKind::game};
  // Direct child must be selected by a runner that actually owns the game/app
  // lifetime. Forwarding launchers and unknown runners never accrue playtime.
  TrackingQuality tracking{TrackingQuality::unknown};
  std::optional<ProviderObservationSpec> observation;
};
struct SessionSnapshot {
  std::string id;
  SessionDescriptor descriptor;
  std::int64_t process_id{-1};
  SessionState state{SessionState::running};
  std::uint64_t started_at{}, ended_at{}, duration_ms{};
  std::optional<int> exit_code;
  std::string tracking_message, persistence_error;
  bool awaiting_observation{};
  SessionCapabilities capabilities;
  std::optional<integrations::SessionWindowTarget> window;
};
struct SessionLaunchResult {
  bool success{};
  std::string session_id, message;
  explicit operator bool() const noexcept { return success; }
};
struct SessionHistory {
  std::vector<SessionSnapshot> recent;
  std::uint64_t launch_count{}, total_playtime_ms{};
  std::uint64_t provider_observed_playtime_ms{};
  std::string error;
};
// Returned snapshots are copies. Observation and persistence run independently
// of UI ticks. Only session-owned journals are written, never library.json.
class SessionManager {
public:
  SessionManager();
  ~SessionManager();
  SessionManager(const SessionManager&) = delete;
  SessionManager& operator=(const SessionManager&) = delete;
  [[nodiscard]] SessionLaunchResult launch(SessionDescriptor descriptor, const std::vector<std::string>& arguments,
                                          const std::filesystem::path& working_directory = {});
  [[nodiscard]] std::vector<SessionSnapshot> snapshot() const;
  [[nodiscard]] std::optional<SessionSnapshot> active_session() const;
  [[nodiscard]] integrations::Result quit(const std::string& session_id,
    integrations::ProcessStopMode mode = integrations::ProcessStopMode::request_close);
  [[nodiscard]] SessionHistory history(const std::filesystem::path& profile_directory, const std::string& identity = {}) const;
  // A forwarding application may become directly observed only when a live
  // window belongs to its already-owned child. Never adopts another PID.
  [[nodiscard]] integrations::Result confirm_application_window(const std::string& session_id,const integrations::SessionWindowTarget& target);
  // Stops observation promptly and records detached sessions. Does not kill or
  // wait for a running game. Call explicitly if persistence errors need UI.
  [[nodiscard]] integrations::Result shutdown();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace openxmb::desktop
