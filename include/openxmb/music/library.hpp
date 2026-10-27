#pragma once
#include "openxmb/music/queue.hpp"
#include <functional>
#include <map>

namespace openxmb::music {
struct LibraryTrack {
  QueueEntry entry;
  std::string album, artist;
  std::uintmax_t size{};
  std::filesystem::file_time_type modified{};
};
using LibraryGroups = std::map<std::string, std::vector<LibraryTrack>>;
std::vector<LibraryTrack> scan_library(const std::filesystem::path& root,
    const std::function<bool()>& cancelled = {});
void sort_library(std::vector<LibraryTrack>& tracks, const std::string& order);
LibraryGroups group_library(const std::vector<LibraryTrack>& tracks, const std::string& group);
std::vector<QueueEntry> collect_tracks(const std::filesystem::path& path);
void save_playlist_atomic(const std::filesystem::path& path, const std::vector<QueueEntry>& tracks);
std::filesystem::path rename_playlist(const std::filesystem::path& path, const std::string& name);
void delete_playlist(const std::filesystem::path& path);

class PlaylistStore {
public:
  explicit PlaylistStore(std::filesystem::path music_root) : root_(std::move(music_root) / "Playlists") {}
  const std::filesystem::path& directory() const { return root_; }
  std::vector<std::filesystem::path> list() const;
  std::filesystem::path create(const std::string& name, const std::vector<QueueEntry>& tracks);
  std::filesystem::path rename(const std::filesystem::path& path, const std::string& name);
  void erase(const std::filesystem::path& path);
private:
  std::filesystem::path named(const std::string& name) const;
  void check_owned(const std::filesystem::path& path) const;
  std::filesystem::path root_;
};
}
