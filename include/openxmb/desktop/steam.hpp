#pragma once

#include "openxmb/desktop/library.hpp"
#include <filesystem>
#include <vector>

namespace openxmb::desktop {
// Explicit roots isolate discovery from default host installations. Reads only
// Steam's registered library roots and their direct app manifests; never runs
// Steam, downloads, repairs or modifies its files. Installed tools/applications
// are included because these manifests do not reliably identify product type.
[[nodiscard]] GameDiscovery discover_steam_games(
  const std::vector<std::filesystem::path>& installation_roots = {},
  const std::filesystem::path& steam_executable = {});
} // namespace openxmb::desktop
