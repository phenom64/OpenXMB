/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025-2026 Syndromatic Ltd. All rights reserved
 * Designed by Kavish Krishnakumar in Manchester.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "openxmb/music/queue.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace openxmb::music;
namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void queues() {
  Queue queue;
  require(!queue.current() && !queue.next(), "empty queue has no selection");
  queue.assign({{"one.mp3", "One"}, {"two.flac", "Two"}, {"three.ogg", "Three"}}, 1);
  require(queue.current()->title == "Two", "requested queue selection retained");
  require(queue.next() && queue.current()->title == "Three", "next advances in sequence");
  require(!queue.next(), "repeat-off stops at queue end");
  queue.set_repeat(Repeat::one);
  require(queue.next(true) && queue.current()->title == "Three", "natural repeat-one stays on track");
  require(!queue.next(false), "manual next does not repeat one");
  queue.set_repeat(Repeat::all);
  require(queue.next() && queue.current()->title == "One", "repeat-all wraps queue");
  require(queue.previous() && queue.current()->title == "Three", "previous wraps in repeat-all");
  queue.set_repeat(Repeat::off);
  queue.select(1);
  queue.set_shuffle(true, 123);
  require(queue.current()->title == "Two", "shuffle keeps current track");
  std::set<std::string> visited{queue.current()->title};
  while (queue.next()) visited.insert(queue.current()->title);
  require(visited.size() == 3, "shuffle visits every track once before ending");
  queue.set_shuffle(false);
  queue.select(1);
  require(queue.move(0, 2) && queue.current()->title == "Two", "queue reorder preserves current identity");
  require(queue.erase(2) && queue.current()->title == "Two", "removing other item preserves current identity");
  require(queue.erase(0) && queue.current()->title == "Three", "removing selected item selects successor");
  require(queue.erase(0) && !queue.current(), "removing final track empties queue");
  require(!queue.erase(0) && !queue.move(0, 1) && !queue.select(5), "invalid queue edits rejected");
}
void playlists(const std::filesystem::path &root) {
  const std::vector<QueueEntry> entries{{root / "a song & more.mp3", "A song"}, {root / "é.flac", "Unicode"}};
  for (const char *name : {"roundtrip.m3u8", "roundtrip.pls"}) {
    const auto file = root / name;
    write_playlist(file, entries);
    const auto read = read_playlist(file);
    require(read.size() == 2 && read[0].path == entries[0].path && read[1].path == entries[1].path, "playlist roundtrip keeps special filenames");
  }
  std::ofstream(root / "relative.m3u") << "#EXTM3U\n#EXTINF:3,Display title\na song & more.mp3\n";
  const auto relative = read_playlist(root / "relative.m3u");
  require(relative[0].path == entries[0].path && relative[0].title == "Display title", "relative entries and titles resolve");
  std::ofstream(root / "remote.m3u") << "https://example.invalid/song.mp3\n";
  bool failed = false;
  try { (void)read_playlist(root / "remote.m3u"); } catch (const std::runtime_error &) { failed = true; }
  require(failed, "network entries are rejected without fetching");
  failed = false;
  try { write_playlist(root / "bad.m3u", {{"line\nbreak.mp3", "bad"}}); } catch (const std::runtime_error &) { failed = true; }
  require(failed, "ambiguous linebreak paths rejected before save");
  require(supported_audio("UPPER.FLAC") && supported_playlist("LIST.M3U8") && !supported_audio("movie.mp4"), "audio recognition handles case and excludes video");
}
}
int main() {
  const auto root = std::filesystem::temp_directory_path() / ("openxmb-music-queue-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    std::filesystem::create_directories(root);
    queues();
    playlists(root);
    std::filesystem::remove_all(root);
    std::cout << "Music queue and playlist checks passed\n";
  } catch (const std::exception &error) {
    std::filesystem::remove_all(root);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
