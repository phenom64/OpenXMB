#include "openxmb/integrations/disc.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <fstream>
#include <set>

#ifdef __linux__
#include <fcntl.h>
#include <linux/cdrom.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace openxmb::integrations {
namespace {
std::filesystem::path case_child(const std::filesystem::path& parent, std::string_view name) {
  std::error_code error;
  for (std::filesystem::directory_iterator it(parent, error), end; !error && it != end; it.increment(error)) {
    auto candidate = it->path().filename().string();
    std::ranges::transform(candidate, candidate.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (candidate == name) return it->path();
  }
  return {};
}
std::filesystem::path largest_stream(const std::filesystem::path& parent, std::string_view extension) {
  std::error_code error;
  std::filesystem::path found;
  std::uintmax_t largest{};
  for (std::filesystem::directory_iterator it(parent, error), end; !error && it != end; it.increment(error)) {
    if (!it->is_regular_file(error)) continue;
    auto ext = it->path().extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (ext != extension) continue;
    const auto bytes = it->file_size(error);
    if (!error && bytes > largest) { largest = bytes; found = it->path(); }
  }
  return found;
}
}
std::string_view disc_kind_name(DiscKind kind) noexcept {
  switch (kind) {
  case DiscKind::absent: return "No disc";
  case DiscKind::audio_cd: return "Audio CD";
  case DiscKind::dvd_video: return "DVD Video";
  case DiscKind::blu_ray: return "Blu-ray";
  case DiscKind::data: return "Data disc";
  }
  return "Unknown disc";
}
DiscInfo probe_disc(const std::filesystem::path& path) {
  DiscInfo info;
  info.path = path;
  info.label = path.filename().string();
  std::error_code error;
  if (std::filesystem::is_directory(path, error)) {
    if (const auto video = case_child(path, "VIDEO_TS"); !video.empty()) {
      info.kind = DiscKind::dvd_video;
      info.representative_stream = largest_stream(video, ".vob");
    } else if (const auto bdmv = case_child(path, "BDMV"); !bdmv.empty()) {
      info.kind = DiscKind::blu_ray;
      info.representative_stream = largest_stream(case_child(bdmv, "STREAM"), ".m2ts");
    } else info.kind = DiscKind::data;
  }
#ifdef __linux__
  else if (std::filesystem::is_block_file(path, error)) {
    const auto fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return info;
    const auto status = ioctl(fd, CDROM_DISC_STATUS, CDSL_CURRENT);
    close(fd);
    if (status == CDS_AUDIO || status == CDS_MIXED) info.kind = DiscKind::audio_cd;
    else if (status == CDS_DATA_1 || status == CDS_DATA_2 || status == CDS_XA_2_1 || status == CDS_XA_2_2) info.kind = DiscKind::data;
    // udev identifies the medium even when the desktop has not mounted it.
    struct stat device{};
    if (info.kind != DiscKind::absent && stat(path.c_str(), &device) == 0) {
      std::ifstream properties("/run/udev/data/b" + std::to_string(major(device.st_rdev)) + ":" + std::to_string(minor(device.st_rdev)));
      std::string property;
      while (std::getline(properties, property)) {
        if (property == "E:ID_CDROM_MEDIA_BD=1") info.kind = DiscKind::blu_ray;
        else if (property == "E:ID_CDROM_MEDIA_DVD=1" && info.kind != DiscKind::blu_ray) info.kind = DiscKind::dvd_video;
        else if (property.starts_with("E:ID_FS_LABEL=")) info.label = property.substr(14);
      }
    }
  }
#endif
  if (info.label.empty()) info.label = std::string{disc_kind_name(info.kind)};
  return info;
}
std::vector<DiscInfo> discover_discs(const std::vector<std::filesystem::path>& requested_roots) {
  auto roots = requested_roots;
  if (roots.empty()) {
#ifdef _WIN32
    const auto drives = GetLogicalDrives();
    for (unsigned index = 0; index < 26; ++index) {
      if ((drives & (1UL << index)) == 0) continue;
      std::string drive{static_cast<char>('A' + index), ':', '\\'};
      if (GetDriveTypeA(drive.c_str()) == DRIVE_CDROM) roots.emplace_back(drive);
    }
#elif defined(__APPLE__)
    roots.emplace_back("/Volumes");
#else
    roots = {"/media", "/run/media", "/mnt"};
    std::error_code error;
    for (std::filesystem::directory_iterator it("/dev", error), end; !error && it != end; it.increment(error))
      if (it->path().filename().string().starts_with("sr")) roots.push_back(it->path());
#endif
  }
  std::set<std::filesystem::path> seen;
  std::vector<DiscInfo> discs;
  const auto consider = [&](const std::filesystem::path& path) {
    auto info = probe_disc(path);
    if (info.kind == DiscKind::absent || info.kind == DiscKind::data || !seen.insert(path).second) return;
    discs.push_back(std::move(info));
  };
  for (const auto& root : roots) {
    consider(root);
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) continue;
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
      if (it.depth() > 2) { it.disable_recursion_pending(); continue; }
      if (it->is_directory(error)) {
        const auto before = discs.size();
        consider(it->path());
        if (discs.size() > before) it.disable_recursion_pending();
      }
    }
  }
  return discs;
}
ProcessResult play_disc(const DiscInfo& disc) {
  if (disc.kind == DiscKind::absent) return {false, -1, "No disc is present"};
  if (auto player = find_executable("mpv")) {
    const auto device_option = disc.kind == DiscKind::blu_ray ? "--bluray-device=" : "--dvd-device=";
    const auto uri = disc.kind == DiscKind::audio_cd ? "cdda://" : disc.kind == DiscKind::blu_ray ? "bd://" : "dvd://";
    std::vector<std::string> args{player->string(), "--force-window=yes"};
    if (disc.kind == DiscKind::audio_cd) args.push_back("--cdda-device=" + disc.path.string());
    else args.push_back(std::string{device_option} + disc.path.string());
    args.emplace_back(uri);
    return launch_process(args);
  }
  if (auto player = find_executable("vlc")) {
    const auto scheme = disc.kind == DiscKind::audio_cd ? "cdda://" : disc.kind == DiscKind::blu_ray ? "bluray://" : "dvd://";
    return launch_process({player->string(), std::string{scheme} + disc.path.string()});
  }
  if (!disc.representative_stream.empty()) {
    if (auto player = find_executable("ffplay")) {
      auto launched = launch_process({player->string(), "-autoexit", disc.representative_stream.string()});
      if (launched) launched.message = "Playing the largest disc stream; install mpv or VLC for disc navigation";
      return launched;
    }
  }
  return {false, -1, "Install mpv or VLC for optical-disc playback. Encrypted media also requires a supported local decoder."};
}
} // namespace openxmb::integrations
