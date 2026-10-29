#pragma once

#include "openxmb/desktop/http.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace openxmb::desktop {
enum class SteamGridDBRole { icon, horizontal_grid, logo, hero };
enum class SteamGridDBOverride { preserve_native, override_native };
struct SteamGridDBGame { std::uint64_t id{}; std::string name; bool exact_match{}; };
struct SteamGridDBAsset {
  std::uint64_t id{}, game_id{};
  SteamGridDBRole role{SteamGridDBRole::icon};
  std::string url, mime;
  unsigned width{}, height{};
};
struct SteamGridDBSearchResult {
  bool success{}, from_cache{}, stale{};
  std::vector<SteamGridDBGame> games;
  std::string message;
  explicit operator bool() const noexcept { return success; }
};
struct SteamGridDBAssetsResult {
  bool success{}, from_cache{}, stale{};
  std::vector<SteamGridDBAsset> assets;
  std::string message;
  unsigned page{};
  bool has_more{};
  explicit operator bool() const noexcept { return success; }
};
struct SteamGridDBDownloadResult {
  bool success{}, from_cache{}, used_native{};
  std::filesystem::path path;
  unsigned width{}, height{};
  std::string message;
  explicit operator bool() const noexcept { return success; }
};
// Call outside the render thread. Keys are passed transiently per request and
// never stored in this object or in cache files. Returned files retain native
// image dimensions/aspect; this backend never changes Library fields. Format
// signatures, container structure and header dimensions are checked before use;
// full compressed-image decoding remains the renderer's responsibility.
class SteamGridDB {
public:
  explicit SteamGridDB(std::filesystem::path cache_root, HttpTransport transport = http_get,
                      std::function<std::uint64_t()> clock = {});
  ~SteamGridDB();
  SteamGridDB(const SteamGridDB&) = delete;
  SteamGridDB& operator=(const SteamGridDB&) = delete;
  [[nodiscard]] SteamGridDBSearchResult search(const std::string& title, const std::string& api_key = {});
  [[nodiscard]] SteamGridDBAssetsResult assets(std::uint64_t game_id, SteamGridDBRole role,
                                              const std::string& api_key = {}, unsigned page = 0);
  // Explicit game/asset IDs enable manual choices. Selection must be present
  // in the requested official result page; no undocumented asset-ID endpoint
  // or fuzzy match is used. Native preservation runs before any provider I/O.
  [[nodiscard]] SteamGridDBDownloadResult select(std::uint64_t game_id, std::uint64_t asset_id, SteamGridDBRole role,
    const std::filesystem::path& native_asset = {}, SteamGridDBOverride policy = SteamGridDBOverride::preserve_native,
    const std::string& api_key = {}, unsigned page = 0);
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace openxmb::desktop
