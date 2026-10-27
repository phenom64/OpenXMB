#include "openxmb/music/library.hpp"
extern "C" {
#include <libavformat/avformat.h>
}
#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace openxmb::music {
namespace {
std::string lower(std::string text) {
  for(auto& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return text;
}
LibraryTrack probe(const std::filesystem::path& path) {
  LibraryTrack track;
  track.entry = {std::filesystem::absolute(path).lexically_normal(), path.stem().string()};
  std::error_code error;
  track.size = std::filesystem::file_size(path, error);
  if(error) track.size = 0;
  track.modified = std::filesystem::last_write_time(path, error);
  AVFormatContext* raw{};
  const auto utf8 = path.u8string();
  if(avformat_open_input(&raw, reinterpret_cast<const char*>(utf8.c_str()), nullptr, nullptr) < 0) return track;
  const auto close = [](AVFormatContext* value) { avformat_close_input(&value); };
  std::unique_ptr<AVFormatContext, decltype(close)> format(raw, close);
  // Tags are available after header parsing for local supported music formats;
  // avoid decoding packets or probing an entire file during library indexing.
  auto tag = [&](const char* key) -> std::string {
    if(auto* value = av_dict_get(raw->metadata, key, nullptr, 0)) return value->value;
    for(unsigned i = 0; i < raw->nb_streams; ++i)
      if(raw->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        if(auto* value = av_dict_get(raw->streams[i]->metadata, key, nullptr, 0)) return value->value;
    return {};
  };
  if(auto title = tag("title"); !title.empty()) track.entry.title = std::move(title);
  track.artist = tag("artist"); track.album = tag("album");
  return track;
}
}
std::vector<LibraryTrack> scan_library(const std::filesystem::path& root, const std::function<bool()>& cancelled) {
  std::vector<LibraryTrack> tracks;
  std::error_code error;
  std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
  for(; !error && it != end; it.increment(error)) {
    if(cancelled && cancelled()) return {};
    if(tracks.size() >= 10000) throw std::runtime_error("Music library exceeds 10000 tracks; choose a smaller folder");
    if(it->path().filename().string().starts_with('.')) { if(it->is_directory(error)) it.disable_recursion_pending(); continue; }
    if(it->is_regular_file(error) && supported_audio(it->path())) tracks.push_back(probe(it->path()));
  }
  sort_library(tracks, "Name");
  return tracks;
}
void sort_library(std::vector<LibraryTrack>& tracks, const std::string& order) {
  std::sort(tracks.begin(), tracks.end(), [&](const auto& a, const auto& b) {
    if(order == "Date" && a.modified != b.modified) return a.modified > b.modified;
    if(order == "Size" && a.size != b.size) return a.size < b.size;
    const auto left = lower(a.entry.title), right = lower(b.entry.title);
    return left == right ? a.entry.path < b.entry.path : left < right;
  });
}
LibraryGroups group_library(const std::vector<LibraryTrack>& tracks, const std::string& group) {
  LibraryGroups groups;
  for(const auto& track : tracks) {
    auto label = group == "Albums" ? (track.album.empty() ? "Unknown Album" : track.album) :
        (track.artist.empty() ? "Unknown Artist" : track.artist);
    groups[label].push_back(track);
  }
  return groups;
}
std::vector<QueueEntry> collect_tracks(const std::filesystem::path& path) {
  std::error_code error;
  if(std::filesystem::is_directory(path, error)) {
    std::vector<QueueEntry> tracks;
    for(auto& track : scan_library(path)) tracks.push_back(std::move(track.entry));
    return tracks;
  }
  if(supported_playlist(path)) return read_playlist(path, true);
  if(!supported_audio(path) || !std::filesystem::is_regular_file(path)) throw std::runtime_error("Choose an existing music track, playlist or folder");
  return {probe(path).entry};
}
void save_playlist_atomic(const std::filesystem::path& path, const std::vector<QueueEntry>& tracks) {
  if(!supported_playlist(path)) throw std::runtime_error("Choose a playlist filename");
  static std::atomic<unsigned> serial{};
  const auto id = std::to_string(++serial);
  auto temporary = path.parent_path() / (".openxmb-save-" + id + path.extension().string());
  auto backup = path.parent_path() / (".openxmb-backup-" + id + path.extension().string());
  bool backed_up = false, temporary_owned = false;
  try {
    if(std::filesystem::exists(temporary) || std::filesystem::exists(backup)) throw std::runtime_error("Playlist save recovery files already exist");
    temporary_owned = true;
    write_playlist(temporary, tracks);
    if(std::filesystem::exists(path)) { std::filesystem::rename(path, backup); backed_up = true; }
    std::filesystem::rename(temporary, path);
    if(backed_up) { std::error_code error; std::filesystem::remove(backup, error); }
  } catch(...) {
    std::error_code error;
    if(temporary_owned) std::filesystem::remove(temporary, error);
    if(backed_up) std::filesystem::rename(backup, path, error);
    throw;
  }
}
namespace {
std::string valid_name(const std::string& name) {
  if(name.empty() || name.size() > 120 || name == "." || name == ".." ||
      name.find_first_of("<>:\"/\\|?*\r\n\t") != std::string::npos ||
      name.back() == '.' || name.back() == ' ' ||
      std::all_of(name.begin(), name.end(), [](unsigned char ch) { return std::isspace(ch); }))
    throw std::runtime_error("Choose a playlist name without path separators or reserved filename characters");
  const auto base = lower(std::filesystem::path(name).stem().string());
  if(base == "con" || base == "prn" || base == "aux" || base == "nul" ||
      (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base.back() >= '0' && base.back() <= '9'))
    throw std::runtime_error("This playlist name is reserved by the operating system");
  return name;
}
}
std::filesystem::path rename_playlist(const std::filesystem::path& path, const std::string& name) {
  if(!supported_playlist(path) || !std::filesystem::is_regular_file(path)) throw std::runtime_error("Select an existing playlist file");
  const auto destination = path.parent_path() / (valid_name(name) + path.extension().string());
  if(destination == path) return path;
  if(std::filesystem::exists(destination)) throw std::runtime_error("A playlist with that name already exists");
  std::filesystem::rename(path,destination);return destination;
}
void delete_playlist(const std::filesystem::path& path) {
  if(!supported_playlist(path) || !std::filesystem::is_regular_file(path)) throw std::runtime_error("Select an existing playlist file");
  if(!std::filesystem::remove(path)) throw std::runtime_error("Playlist no longer exists");
}
std::filesystem::path PlaylistStore::named(const std::string& name) const {
  return root_ / (valid_name(name) + ".m3u8");
}
void PlaylistStore::check_owned(const std::filesystem::path& path) const {
  if(std::filesystem::absolute(path).lexically_normal().parent_path() != std::filesystem::absolute(root_).lexically_normal() || !supported_playlist(path))
    throw std::runtime_error("Select a playlist from your Playlists folder");
}
std::vector<std::filesystem::path> PlaylistStore::list() const {
  std::vector<std::filesystem::path> paths;
  std::error_code error;
  for(std::filesystem::directory_iterator it(root_, error), end; !error && it != end; it.increment(error))
    if(it->is_regular_file(error) && supported_playlist(it->path()) && !it->path().filename().string().starts_with('.')) paths.push_back(it->path());
  std::sort(paths.begin(), paths.end()); return paths;
}
std::filesystem::path PlaylistStore::create(const std::string& name, const std::vector<QueueEntry>& tracks) {
  auto path = named(name);
  std::filesystem::create_directories(root_);
  if(std::filesystem::exists(path)) throw std::runtime_error("A playlist with that name already exists");
  save_playlist_atomic(path, tracks); return path;
}
std::filesystem::path PlaylistStore::rename(const std::filesystem::path& path, const std::string& name) {
  check_owned(path); return rename_playlist(path,name);
}
void PlaylistStore::erase(const std::filesystem::path& path) {
  check_owned(path);
  delete_playlist(path);
}
}
