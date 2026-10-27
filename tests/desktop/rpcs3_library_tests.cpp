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

#include "openxmb/desktop/library.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace openxmb::desktop;
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void sfo(const fs::path &path, const std::map<std::string, std::string> &values) {
  std::vector<unsigned char> keys, data;
  struct Entry { unsigned key, length, offset; };
  std::vector<Entry> entries;
  for (const auto &[key, value] : values) {
    entries.push_back({static_cast<unsigned>(keys.size()), static_cast<unsigned>(value.size() + 1), static_cast<unsigned>(data.size())});
    keys.insert(keys.end(), key.begin(), key.end()); keys.push_back(0);
    data.insert(data.end(), value.begin(), value.end()); data.push_back(0);
    while (data.size() % 4) data.push_back(0);
  }
  const unsigned key_offset = 20 + entries.size() * 16;
  const unsigned data_offset = (key_offset + keys.size() + 3) & ~3u;
  std::ofstream output(path, std::ios::binary);
  auto word = [&](std::uint32_t value, unsigned size) { for (unsigned i = 0; i < size; ++i) output.put(static_cast<char>(value >> (i * 8))); };
  output.write("\0PSF", 4); word(0x101, 4); word(key_offset, 4); word(data_offset, 4); word(entries.size(), 4);
  for (const auto &entry : entries) { word(entry.key, 2); word(0x0204, 2); word(entry.length, 4); word((entry.length + 3) & ~3u, 4); word(entry.offset, 4); }
  output.write(reinterpret_cast<const char *>(keys.data()), keys.size());
  while (static_cast<unsigned>(output.tellp()) < data_offset) output.put(0);
  output.write(reinterpret_cast<const char *>(data.data()), data.size());
}
void variable(const char *name, const std::string &value) {
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}
}
int main() {
  const auto root = fs::temp_directory_path() / ("openxmb-rpcs3-library-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const std::string original_emulator = std::getenv("RPCS3_EXECUTABLE") ? std::getenv("RPCS3_EXECUTABLE") : "";
  try {
    const auto game_root = root / "Fixture Disc";
    const auto folder = game_root / "PS3_GAME";
    fs::create_directories(folder / "USRDIR");
    sfo(folder / "PARAM.SFO", {{"TITLE", "Local PS3 Fixture"}, {"TITLE_ID", "TEST00001"}, {"APP_VER", "01.00"}, {"CATEGORY", "DG"}});
    for (const char *name : {"ICON0.PNG", "PIC1.PNG", "ICON1.PAM", "SND0.AT3", "USRDIR/EBOOT.BIN"}) std::ofstream(folder / name) << "fixture";
    const auto metadata = read_param_sfo(folder / "PARAM.SFO");
    require(metadata.at("TITLE") == "Local PS3 Fixture" && metadata.at("TITLE_ID") == "TEST00001", "PARAM.SFO table parser reads bounded title metadata");
#ifndef _WIN32
    const auto executable = root / "fixture-rpcs3";
    std::ofstream(executable) << "#!/bin/sh\nprintf '%s\\n%s' \"$1\" \"$2\" > emulator-launch.txt\n";
    fs::permissions(executable, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);
    variable("RPCS3_EXECUTABLE", executable.string());
    require(rpcs3_executable() == executable, "configured installed emulator is discovered without launching");
#endif
    const auto game = import_rpcs3_game(game_root);
    require(game.title == "Local PS3 Fixture" && game.title_id == "TEST00001" && game.platform == "PlayStation 3 (RPCS3)", "RPCS3 disc folder imports title and platform");
    require(game.icon == folder / "ICON0.PNG" && game.background == folder / "PIC1.PNG" && game.preview == folder / "ICON1.PAM" && game.music == folder / "SND0.AT3", "RPCS3 icon/background/movie/music are selected from native metadata layout");
    require(game.arguments.size() == 2 && game.arguments[1] == (folder / "USRDIR/EBOOT.BIN").string(), "emulator target is supplied as a literal boot-path argument");
#ifndef _WIN32
    launch_game(game);
    const auto output = folder / "emulator-launch.txt";
    for (int i = 0; i < 100 && !fs::exists(output); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::ifstream input(output); std::string flag, boot; std::getline(input, flag); std::getline(input, boot);
    require(flag == "--no-gui" && boot == game.arguments[1], "installed emulator launch receives exact RPCS3 arguments and working directory");
#endif
    Library library(root / "library"); library.add_game_root(root);
    require(library.scan_games() >= 1, "game scan discovers an RPCS3 disc folder");
    const auto games = library.games();
    require(std::ranges::count_if(games, [](const Game &item) { return item.title_id == "TEST00001"; }) == 1, "scan does not import internal PS3 binaries as duplicate games");
    std::ofstream(folder / "broken.sfo", std::ios::binary) << "\0PSF";
    bool rejected = false;
    try { (void)read_param_sfo(folder / "broken.sfo"); } catch (const std::runtime_error &) { rejected = true; }
    require(rejected, "truncated PARAM.SFO data is rejected");
    variable("RPCS3_EXECUTABLE", original_emulator); fs::remove_all(root);
    std::cout << "RPCS3 metadata, media discovery and external launch checks passed\n";
  } catch (const std::exception &error) { variable("RPCS3_EXECUTABLE", original_emulator); fs::remove_all(root); std::cerr << error.what() << '\n'; return 1; }
}
