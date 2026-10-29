#pragma once

#include "openxmb/desktop/library.hpp"
#include "openxmb/desktop/session.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace openxmb::desktop {
struct RunnerDiscoveryOptions {
  std::map<std::string, std::filesystem::path> executable_overrides;
  std::vector<std::filesystem::path> search_roots;
  bool search_path{true}, search_defaults{true};
};
// Reads only runner.<known-id>.path. Empty values restore normal discovery;
// all unrelated preferences are ignored. This function performs no I/O.
[[nodiscard]] RunnerDiscoveryOptions runner_options_from_preferences(const std::map<std::string, std::string>& preferences);
struct RunnerDescriptor {
  std::string id, name;
  std::filesystem::path executable;
  TrackingQuality tracking{TrackingQuality::unknown};
  bool requires_content{}, requires_core{};
  std::string message;
  explicit operator bool() const noexcept { return !executable.empty(); }
};
// Returns all seven supported runners, including actionable missing entries.
// Probes local files only, never starts an emulator, installs or downloads.
[[nodiscard]] std::vector<RunnerDescriptor> discover_runners(const RunnerDiscoveryOptions& options = {});
struct LaunchPlan {
  bool success{};
  std::string runner, message;
  std::vector<std::string> arguments; // Includes executable as argv[0].
  std::filesystem::path working_directory;
  TrackingQuality tracking{TrackingQuality::unknown};
  explicit operator bool() const noexcept { return success; }
};
// Native entries preserve explicit executable/argv/cwd. Named runners validate
// content/core and generate defaults only when no explicit argv is supplied.
// fullscreen requests the runner's supported startup option; absence leaves
// the emulator's own configuration in control where no inverse flag exists.
[[nodiscard]] LaunchPlan plan_game_launch(const Game& game, const RunnerDiscoveryOptions& options = {}, bool fullscreen = true);
} // namespace openxmb::desktop
