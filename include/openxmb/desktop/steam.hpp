#pragma once

#include "openxmb/desktop/library.hpp"
#include <filesystem>
#include <vector>

namespace openxmb::desktop {
// Explicit roots isolate discovery from default host installations. Reads only
// Steam's registered library roots and their direct app manifests; never runs
// Steam, downloads, repairs or modifies its files. Local appinfo product types
// exclude support applications; absent/unsupported metadata retains unknown
// games conservatively.
[[nodiscard]] GameDiscovery discover_steam_games(
  const std::vector<std::filesystem::path>& installation_roots = {},
  const std::filesystem::path& steam_executable = {});
// Non-destructive games-view/launch policy. Native user imports are unaffected.
// Uses bounded cached local provider metadata and known support-only app IDs;
// it never removes or rewrites the stored library entry or its artwork.
[[nodiscard]] bool steam_game_is_playable(const Game& game);
} // namespace openxmb::desktop
