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
#include <optional>
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
struct Environment {
  std::map<std::string, std::optional<std::string>> original;
  Environment() {
    for (const char *name : {"RPCS3_EXECUTABLE", "RPCS3_DATA_DIR", "RPCS3_CONFIG_DIR", "XDG_CONFIG_HOME", "HOME", "APPDATA", "PATH"}) {
      const auto *value = std::getenv(name);
      original[name] = value ? std::optional<std::string>{value} : std::nullopt;
      variable(name, "");
    }
  }
  ~Environment() {
    for (const auto &[name, value] : original) {
      if (value) variable(name.c_str(), *value);
      else {
#ifdef _WIN32
        _putenv_s(name.c_str(), "");
#else
        unsetenv(name.c_str());
#endif
      }
    }
  }
};
std::string utf8(const fs::path &path) {
  const auto value = path.generic_u8string();
  return {value.begin(), value.end()};
}
std::string quoted_path(const fs::path &path, char quote) {
  std::string result(1, quote);
  const auto native = path.u8string();
  const auto value = quote == '"' ? std::string(native.begin(), native.end()) : utf8(path);
  for (const auto c : value) {
    if (quote == '\'' && c == '\'') result += '\'';
    if (quote == '"' && (c == '"' || c == '\\')) result += '\\';
    result += c;
  }
  return result + quote;
}
void file(const fs::path &path, const std::string &value = "fixture") {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << value;
}
void title(const fs::path &folder, const std::string &id, const std::string &category, const std::string &name = "Fixture Title", bool bootable = true) {
  fs::create_directories(folder / "USRDIR");
  sfo(folder / "PARAM.SFO", {{"TITLE", name}, {"TITLE_ID", id}, {"APP_VER", "01.00"}, {"CATEGORY", category}});
  if (bootable) file(folder / "USRDIR/EBOOT.BIN");
}
fs::path registry(const fs::path &data) {
#ifdef _WIN32
  return data / "config/games.yml";
#else
  return data / "games.yml";
#endif
}
const Game &by_title(const std::vector<Game> &games, const std::string &id) {
  const auto found = std::ranges::find_if(games, [&](const Game &game) { return game.title_id == id; });
  require(found != games.end(), "expected discovered title is present");
  return *found;
}
void registered_discovery(const fs::path &root) {
  const auto data = root / "emulator data";
  variable("RPCS3_DATA_DIR", data.string());
  const auto disc = root / "External Disc" / "PS3_GAME";
#ifdef _WIN32
  const auto apostrophe = root / "Player's game # edition";
#else
  const auto apostrophe = root / "Player's game: edition";
#endif
  const auto unicode = root / fs::path(std::u8string(u8"Unicode \u6771\u4eac \U0001f3ae"));
  title(disc, "TEST00001", "DG");
  title(apostrophe, "TEST00002", "HG");
  title(unicode, "TEST00003", "HG");
  title(data / "dev_hdd0/game/TEST00004", "TEST00004", "HG");
  title(data / "games/Other Disc/PS3_GAME", "TEST00005", "DG");
  title(data / "dev_hdd0/game/TEST00006", "TEST00006", "GD");
  title(data / "dev_hdd0/game/TEST00007", "TEST00007", "HG", "No boot executable", false);
  title(data / "dev_hdd0/game/TEST00008", "TEST00008", "AC");
  const auto broken = root / "Broken";
  file(broken / "PARAM.SFO", "invalid"); file(broken / "USRDIR/EBOOT.BIN");
  const auto oversized = root / "Oversized";
  file(oversized / "PARAM.SFO");
  { std::ofstream output(oversized / "PARAM.SFO", std::ios::binary); output.seekp(1024 * 1024); output.put(0); }
  file(oversized / "USRDIR/EBOOT.BIN");
  file(root / "unsupported.iso");
  // yaml-cpp's std::map emitter uses plain, single-quoted and double-quoted
  // scalar strings. The Windows drive colon is part of a plain scalar.
  auto contents = "TEST00001: " + utf8(disc.parent_path()) + "\n";
  contents += "DUPLICATE: " + quoted_path(disc / "USRDIR/EBOOT.BIN", '\'') + "\n";
  contents += "SFO_DUPLICATE: " + quoted_path(disc / "PARAM.SFO", '"') + "\n";
  contents += "'TEST00002': " + quoted_path(apostrophe, '\'') + "\n";
  auto escaped_unicode = quoted_path(unicode, '"');
  const auto suffix = escaped_unicode.find("Unicode ");
  escaped_unicode.replace(suffix, escaped_unicode.size() - suffix, "\\x55nicode \\u6771\\u4eac \\U0001f3ae\"");
  contents += "\"TEST00003\": " + escaped_unicode + "\n";
  contents += "RAW_UNICODE_DUPLICATE: " + utf8(unicode) + "\n";
  contents += "MISSING: " + quoted_path(root / "does not exist", '"') + "\n";
  contents += "BROKEN: " + quoted_path(broken, '"') + "\n";
  contents += "OVERSIZED: " + quoted_path(oversized, '"') + "\n";
  contents += "ARCHIVE: " + quoted_path(root / "unsupported.iso", '"') + "\n";
  file(registry(data), contents);
  Library library(root / "library");
  const auto discovered=discover_games();
  require(!discovered.empty()&&library.games().empty(),"background discovery reads games without writing the profile");
  require(library.import_games(discovered) == 5, "automatic discovery imports only bootable registered/native DG and HG titles");
  auto games = library.games();
  require(games.size() == 5 && by_title(games, "TEST00003").game_directory == fs::weakly_canonical(unicode), "quoted Unicode registry paths decode without loss");
  require(by_title(games, "TEST00002").game_directory == fs::weakly_canonical(apostrophe), "single-quoted apostrophes and path punctuation decode");
  require(by_title(games, "TEST00004").game_directory == fs::weakly_canonical(data / "dev_hdd0/game/TEST00004") &&
    by_title(games, "TEST00005").game_directory == fs::weakly_canonical(data / "games/Other Disc/PS3_GAME"), "native HDD and disc installation roots both provide bootable titles");
  library.add_game_root(disc.parent_path());
  require(library.scan_games() == 0 && library.games().size() == 5, "registry file/root/boot aliases and manual roots deduplicate");
  bool rejected = false;
  try { (void)import_rpcs3_game(data / "dev_hdd0/game/TEST00006"); } catch (const std::runtime_error &) { rejected = true; }
  require(rejected, "GD patch data cannot be imported as a bootable game");
  rejected = false;
  try { (void)read_param_sfo(oversized / "PARAM.SFO"); } catch (const std::runtime_error &) { rejected = true; }
  require(rejected, "oversized SFO metadata is rejected");
}
void supported_locations(const fs::path &root) {
  const auto external = root / "External"; title(external, "TEST00012", "HG");
  const auto contents = "TEST00012: " + quoted_path(external, '"') + "\n";
  variable("RPCS3_DATA_DIR", ""); variable("RPCS3_EXECUTABLE", "");
#ifdef _WIN32
  const auto data = root / fs::path(std::u8string(u8"override \u6771\u4eac"));
  _wputenv_s(L"RPCS3_CONFIG_DIR", data.c_str());
  file(registry(data), contents);
  Library configured(root / "configured-library");
  require(configured.scan_games() == 1, "Unicode Windows RPCS3_CONFIG_DIR/config/games.yml is discovered");
  variable("RPCS3_CONFIG_DIR", "");
  const auto executable = root / "installation/rpcs3.exe"; file(executable);
  variable("RPCS3_EXECUTABLE", executable.string());
  file(executable.parent_path() / "games.yml", contents);
  Library legacy(root / "legacy-library");
  require(legacy.scan_games() == 1, "legacy Windows installation games.yml is read without migrating it");
  file(registry(executable.parent_path() / "portable"), contents);
  Library portable(root / "portable-library");
  require(portable.scan_games() == 1, "portable/config/games.yml is discovered beside the installed emulator");
  variable("RPCS3_EXECUTABLE", "");
#elif defined(__APPLE__)
  const auto home = root / "home"; variable("HOME", home.string());
  file(home / "Library/Application Support/rpcs3/games.yml", contents);
  Library native(root / "native-library");
  require(native.scan_games() == 1, "macOS native RPCS3 registry is discovered");
  variable("HOME", "");
#else
  const auto config = root / "xdg"; variable("XDG_CONFIG_HOME", config.string());
  file(config / "rpcs3/games.yml", contents);
  Library native(root / "native-library");
  require(native.scan_games() == 1, "XDG RPCS3 registry is discovered");
  variable("XDG_CONFIG_HOME", "");
  const auto home = root / "home"; variable("HOME", home.string());
  file(home / ".var/app/net.rpcs3.RPCS3/config/rpcs3/games.yml", contents);
  Library flatpak(root / "flatpak-library");
  require(flatpak.scan_games() == 1, "Flatpak RPCS3 registry is discovered");
  variable("HOME", "");
#endif
}
void refresh_preserves_choices(const fs::path &root) {
  const auto data = root / "data";
  const auto folder = data / "dev_hdd0/game/TEST00009";
  variable("RPCS3_DATA_DIR", data.string());
  title(folder, "TEST00009", "HG", "Old title");
  for (const char *name : {"ICON0.PNG", "PIC1.PNG", "ICON1.PAM", "SND0.AT3"}) file(folder / name);
  Library library(root / "library");
  require(library.scan_games() == 1, "native HDD title is discovered without a manual root");
  auto selected = library.games().front(); const auto id = selected.id;
  selected.favorite = true; selected.play_count = 7; selected.last_played = 12345678;
  selected.icon = root / "user icon.png"; selected.background = root / "user background.png";
  selected.preview = root / "user preview.pam"; selected.music = root / "user music.at3";
  library.save_game(selected);
  title(folder, "TEST00009", "HG", "Refreshed title");
  const auto other_emulator = root / "other-rpcs3.exe"; file(other_emulator);
#ifndef _WIN32
  fs::permissions(other_emulator, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);
#endif
  variable("RPCS3_EXECUTABLE", other_emulator.string());
  require(library.scan_games() == 0, "emulator executable changes do not create a duplicate title");
  auto refreshed = library.games().front();
  require(refreshed.id == id && refreshed.title == "Refreshed title" && refreshed.executable == other_emulator, "refresh updates native metadata under the stable saved identity");
  require(refreshed.favorite && refreshed.play_count == 7 && refreshed.last_played == 12345678, "refresh preserves favorite and play history");
  require(refreshed.icon == selected.icon && refreshed.background == selected.background && refreshed.preview == selected.preview && refreshed.music == selected.music, "refresh preserves all user-selected artwork and media");
  refreshed.icon = folder / "ICON0.PNG"; refreshed.background = folder / "PIC1.PNG";
  library.save_game(refreshed);
  fs::remove(folder / "ICON0.PNG"); fs::remove(folder / "PIC1.PNG"); file(folder / "PIC0.PNG");
  require(library.scan_games() == 0, "native media refresh does not add a game");
  refreshed = library.games().front();
  require(refreshed.icon.empty() && refreshed.background == folder / "PIC0.PNG", "native artwork refreshes when removed or replaced by fallback media");
}
void invalid_registry(const fs::path &root) {
  const auto data = root / "data";
  const auto external = root / "External";
  variable("RPCS3_DATA_DIR", data.string());
  title(external, "TEST00010", "DG");
  const auto prefix = "TEST00010: " + quoted_path(external, '"') + "\n";
  const std::vector<std::string> invalid{
    "BAD: [one, two]\n", "BAD: {path: anything}\n", "BAD: &anchor /path\n", "BAD: *anchor\n",
    "BAD: !!str /path\n", "BAD: |\n  /path\n", "BAD: >-\n  /path\n", "BAD: \"unterminated\n",
    "BAD: \"bad\\qescape\"\n", "BAD: \"\\uD800\"\n", "BAD: \"\\U00110000\"\n", "BAD: \"\\0\"\n",
    "BAD: \"\xff\"\n", "BAD: one: two\n", "? complex\n: key\n",
    "TEST00010: /duplicate-key\n", "BAD: " + std::string(32769, 'x') + "\n"
  };
  for (std::size_t i = 0; i < invalid.size(); ++i) {
    file(registry(data), prefix + invalid[i]);
    Library library(root / ("library-" + std::to_string(i)));
    require(library.scan_games() == 0 && library.games().empty(), "unsupported/malformed registry fails closed without partially importing entries");
  }
  file(registry(data), std::string(4 * 1024 * 1024 + 1, '#'));
  Library oversized(root / "oversized-library");
  require(oversized.scan_games() == 0, "registry over 4 MiB is rejected before parsing");
  std::string excessive = prefix;
  for (unsigned i = 0; i < 10000; ++i) excessive += "KEY" + std::to_string(i) + ": /nonexistent\n";
  file(registry(data), excessive);
  require(oversized.scan_games() == 0, "registry over 10000 keys is rejected without a partial import");
  title(data / "dev_hdd0/game/TEST00011", "TEST00011", "HG");
  require(oversized.scan_games() == 1, "invalid registry does not prevent independent native installed-game discovery");
}
void scan_save_errors(const fs::path &root, bool registered) {
  const auto data = root / "data";
  variable("RPCS3_DATA_DIR", data.string());
  variable("RPCS3_EXECUTABLE", "");
  const auto folder = registered ? root / "External" : data / "dev_hdd0/game";
  title(folder, "TEST00013", "HG");
  if (registered) file(registry(data), "TEST00013: " + quoted_path(folder, '"') + "\n");
  Library library(root / "library");
  const auto profile = library.active_directory();
  // A regular file in place of the profile directory makes library.json
  // absent/readable-as-empty but prevents creating its save directory. This
  // fails deterministically on Windows and POSIX, including privileged runs.
  fs::remove_all(profile);
  file(profile, "save destination blocked");
  require(library.games().empty(), "blocked save fixture still permits the initial empty library read");
  bool rejected = false;
  try { (void)library.scan_games(); } catch (const std::exception &) { rejected = true; }
  require(rejected, registered ? "registry discovery must propagate library save failures" : "direct native root discovery must propagate library save failures");
  require(fs::is_regular_file(profile), "failed scan leaves the blocked save destination unchanged");
  // The same obstruction must remain harmless when the external metadata is
  // invalid: metadata errors are skipped before any attempted library save.
  file(folder / "PARAM.SFO", "broken metadata");
  require(library.scan_games() == 0, "invalid native metadata is still skipped without attempting a save");
}
}
int main() {
  // Windows TEMP may use an 8.3 user-directory alias. Build expectations from
  // its canonical directory, matching the importer's stable path identity.
  const auto root = fs::weakly_canonical(fs::temp_directory_path()) / ("openxmb-rpcs3-library-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Environment environment;
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
    registered_discovery(root / "registered");
    supported_locations(root / "locations");
    refresh_preserves_choices(root / "refresh");
    invalid_registry(root / "invalid");
    scan_save_errors(root / "registry-save-error", true);
    scan_save_errors(root / "native-save-error", false);
    fs::remove_all(root);
    std::cout << "RPCS3 metadata, media discovery and external launch checks passed\n";
  } catch (const std::exception &error) { fs::remove_all(root); std::cerr << error.what() << '\n'; return 1; }
}
