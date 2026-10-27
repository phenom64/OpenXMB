#include "openxmb/music/library.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace openxmb::music;
void check(bool value, const char *message) { if(!value) throw std::runtime_error(message); }
int main(int argc, char **argv) {
  const auto root = std::filesystem::temp_directory_path() / ("openxmb-library-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    std::filesystem::create_directories(root);
    PlaylistStore playlists(root);
    auto path = playlists.create("Evening Mix", {});
    check(path == root / "Playlists/Evening Mix.m3u8", "playlist storage is stable under music root");
    check(read_playlist(path, true).empty(), "named empty playlist loads for editing");
    bool rejected = false;
    try { playlists.create("Evening Mix", {}); } catch(...) { rejected = true; }
    check(rejected, "duplicate create cannot overwrite playlist");
    for(const auto *name : {"../escape", "CON", "bad:name", "trailing.", " ", "a/b"}) {
      rejected = false;
      try { playlists.create(name, {}); } catch(...) { rejected = true; }
      check(rejected, "invalid playlist name rejected");
    }
    std::vector<QueueEntry> tracks{{root / "one.wav", "One"}, {root / "two.m4a", "Two"}};
    save_playlist_atomic(path, tracks);
    check(read_playlist(path).size() == 2, "playlist round-trips saved tracks");
    path = playlists.rename(path, "Night Mix");
    check(path.filename() == "Night Mix.m3u8" && playlists.list().size() == 1, "rename keeps playlist discoverable");
    rejected = false;
    try { save_playlist_atomic(path, {{root / "bad\nfile.wav", "bad"}}); } catch(...) { rejected = true; }
    check(rejected && read_playlist(path).size() == 2, "failed save preserves previous playlist");
    playlists.erase(path);
    check(playlists.list().empty(), "delete removes only selected playlist");
    const auto imported=root/"Imported.pls";
    save_playlist_atomic(imported,tracks);
    const auto renamed=rename_playlist(imported,"Renamed Import");
    check(renamed.extension()==".pls" && read_playlist(renamed).size()==2,"imported playlist rename preserves its format and contents");
    delete_playlist(renamed);
    check(!std::filesystem::exists(renamed),"explicit delete also supports imported playlists");
    Queue queue;
    queue.assign(tracks); queue.set_shuffle(true, 42);
    const auto current = queue.current()->path;
    queue.insert_next({root / "next.flac", "Next"});
    check(queue.current()->path == current && queue.next() && queue.current()->title == "Next", "Play Next preserves current and inserts into shuffled traversal");
    if(argc > 1) {
      auto library = scan_library(argv[1]);
      check(library.size() == 3, "recursive tagged library finds exactly supported audio");
      check(library[0].artist == "Artist A" && library[0].album == "Album A", "music grouping reads actual tags");
      check(group_library(library, "Albums").size() == 2 && group_library(library, "Artists").size() == 2, "album and artist groups derive from tags");
      check(scan_library(argv[1], [] { return true; }).empty(), "scan cancellation is honored");
      auto entries = collect_tracks(argv[1]);
      check(entries.size() == 3, "Add Folder recursively resolves audio tracks");
      sort_library(library, "Size");
      check(library.front().size <= library.back().size, "Size sort orders real file sizes");
      sort_library(library, "Date");
      check(library.front().modified >= library.back().modified, "Date sort is newest first");
    }
    std::filesystem::remove_all(root);
    std::cout << "Music library, playlist storage and Play Next passed\n";
  } catch(const std::exception& error) {
    std::filesystem::remove_all(root);
    std::cerr << error.what() << '\n'; return 1;
  }
}
