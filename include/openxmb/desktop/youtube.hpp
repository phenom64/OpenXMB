#pragma once
#include "openxmb/desktop/http.hpp"
#include "openxmb/desktop/session.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace openxmb::desktop {
enum class YoutubePlatform { windows, linux, macos };
enum class YoutubeArch { x64, arm64, unknown };
struct YoutubeDetectionOptions {
  std::filesystem::path data_root, executable_override;
  std::vector<std::filesystem::path> search_roots;
  bool search_defaults{true};
};
struct YoutubeApplication {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  TrackingQuality tracking{TrackingQuality::launcher_only};
  bool managed{};
  std::string message;
  std::string version; // Installed version, or empty when unknown.
};
struct YoutubeDetectionResult {
  std::optional<YoutubeApplication> value; std::string error;
  explicit operator bool() const noexcept {return value.has_value()&&error.empty();}
};
struct YoutubeRelease {
  std::string tag, asset_name, url, sha256;
  std::uint64_t size{};
  YoutubePlatform platform{}; YoutubeArch arch{YoutubeArch::unknown};
  bool install_supported{};
};
struct YoutubeReleaseResult {
  std::optional<YoutubeRelease> value; std::string error;
  explicit operator bool() const noexcept {return value.has_value()&&error.empty();}
};
struct YoutubeInstallResult {
  std::filesystem::path executable; std::string error;
  explicit operator bool() const noexcept {return !executable.empty()&&error.empty();}
};
using YoutubeProgress=std::function<bool(std::string_view stage, std::uint64_t downloaded, std::uint64_t total)>;
inline constexpr std::string_view youtube_information="VacuumTube YouTube Leanback client";
[[nodiscard]] YoutubePlatform youtube_platform();
[[nodiscard]] YoutubeArch youtube_arch();
[[nodiscard]] YoutubeDetectionResult detect_youtube(const YoutubeDetectionOptions& options);
[[nodiscard]] YoutubeReleaseResult select_youtube_release(std::string_view metadata, YoutubePlatform platform, YoutubeArch arch);
[[nodiscard]] YoutubeReleaseResult fetch_youtube_release(YoutubePlatform platform, YoutubeArch arch, const HttpTransport& transport);
// Installs the host platform's official archive (Windows portable ZIP, Linux
// tarball, macOS application ZIP) into the managed per-profile directory. No
// host application is launched by this function.
[[nodiscard]] YoutubeInstallResult install_youtube(const std::filesystem::path& data_root,const YoutubeRelease& release,const HttpTransport& transport,const YoutubeProgress& progress);
namespace detail {
// Exposed for fixtures: portable SHA-256 and the bounded extractor for any
// platform's layout. Throws std::runtime_error with a user-facing message.
[[nodiscard]] std::string youtube_sha256(std::string_view bytes);
void extract_youtube_archive(std::string_view bytes,YoutubePlatform platform,YoutubeArch arch,const std::filesystem::path& stage);
} // namespace detail
} // namespace openxmb::desktop


