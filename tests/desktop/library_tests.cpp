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

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

using namespace openxmb::desktop;
namespace fs = std::filesystem;
namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void profiles(const fs::path &root) {
  Library library(root);
  require(library.profiles().size() == 1 && library.active_profile().id == "default", "default profile initialized");
  nlohmann::json baseline{{"shell", {{"background-type", "original"}, {"font-path", "custom.ttf"}}}};
  std::ofstream(root / "baseline.json") << baseline;
  auto second = library.create_profile("User Two", root / "baseline.json");
  require(fs::is_directory(library.themes_path(second.id)), "each profile has an isolated theme directory");
  nlohmann::json copied; std::ifstream(library.config_path(second.id)) >> copied;
  require(copied == baseline, "profile creation preserves baseline settings");
  second.name = "Renamed User"; library.update_profile(second);
  require(library.profiles()[1].name == "Renamed User", "profile rename persisted");
  library.save_friend({{}, "Local Friend", "Test status", true});
  library.switch_profile(second.id);
  require(library.friends().empty(), "local social collections are profile scoped");
  auto account = library.account(); account.biography = "Offline biography"; library.save_account(account);
  require(Library(root).account().biography == "Offline biography", "local account survives reopening");
  auto trophy = library.save_trophy({{}, "Local milestone", "Fixture Game", "Finished fixture", "gold", true, 0});
  require(trophy.unlocked_at > 0 && library.trophies()[0].unlocked, "local trophy unlock gets persisted timestamp");
  trophy.unlocked = false; library.save_trophy(trophy);
  require(library.trophies()[0].unlocked_at == 0, "relocking clears local timestamp");
  library.remove_profile(second.id);
  require(library.active_profile().id == "default" && library.friends().size() == 1, "deleting active profile restores retained profile");
  require(!fs::exists(library.profile_directory(second.id)) && !fs::is_empty(root / "profile-trash"), "deleted profile data is archived recoverably");
  bool rejected = false;
  try { library.remove_profile("default"); } catch (const std::runtime_error &) { rejected = true; }
  require(rejected, "last profile deletion rejected");
  rejected = false;
  try { (void)library.profile_directory("../outside"); } catch (const std::runtime_error &) { rejected = true; }
  require(rejected, "profile identifier cannot escape storage root");
}
void games(const fs::path &root) {
  Library library(root);
  const auto folder = root / "fixture games"; fs::create_directories(folder);
  const auto executable = folder / "fixture.sh";
  std::ofstream(executable) << "#!/bin/sh\nprintf '%s' \"$1\" > launch-result.txt\n";
  fs::permissions(executable, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
  nlohmann::json manifest{{"title", "Fixture Game"}, {"executable", "fixture.sh"}, {"working_directory", "."}, {"genre", "Fixture"}, {"year", 2026}, {"icon", "cover.png"}};
  std::ofstream(folder / "openxmb-game.json") << manifest;
  library.add_game_root(folder); library.add_game_root(folder);
  require(library.game_roots().size() == 1, "duplicate canonical roots deduplicated");
  require(library.scan_games() == 1 && library.scan_games() == 0, "scan imports one game and repeat scan deduplicates");
  auto game = library.games().front();
  require(game.title == "Fixture Game" && game.genre == "Fixture" && game.icon == fs::weakly_canonical(folder / "cover.png"), "manifest metadata and relative media resolve");
  game.arguments = {"literal ; $(touch injected) & spaced value"};
  game.favorite = true;
  library.save_game(game);
#ifndef _WIN32
  launch_game(game);
  const auto output = folder / "launch-result.txt";
  for (int retry = 0; retry < 100 && !fs::exists(output); ++retry) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  std::ifstream input(output); std::string value; std::getline(input, value);
  require(value == game.arguments[0] && !fs::exists(folder / "injected"), "launch preserves literal argv metacharacters and working directory");
#endif
  library.mark_played(game.id);
  require(library.games().front().favorite && library.games().front().play_count == 1, "favorites and play history persist");
  const auto section = library.save_section({{}, "Fixture Section", folder, {}});
  require(library.custom_sections()[0].folder == fs::weakly_canonical(folder), "custom section points at source folder");
  library.remove_section(section.id);
  require(library.custom_sections().empty() && fs::exists(executable), "section removal preserves source folder and files");
  library.remove_game(game.id);
  require(library.games().empty() && fs::exists(executable), "removing library entry preserves game executable");
  bool blocked = false;
  try { (void)library.fetch_artwork(game.id); } catch (const std::runtime_error &) { blocked = true; }
  require(blocked && !library.artwork_provider_available(), "optional artwork requests require explicit provider/key configuration");
}
void runner_manifests(const fs::path& root) {
  Library library(root);
  const auto folder=root/"manifests";fs::create_directories(folder);
  for(const auto* id:{"100","200"}) {
    const nlohmann::json manifest{{"title",std::string("Steam ")+id},{"runner","steam"},{"content",id}};
    std::ofstream(folder/(std::string(id)+".openxmb-game.json"))<<manifest;
  }
  library.add_game_root(folder);
  require(library.scan_games()==2,"runner manifests without an executable remain distinct");
  const auto games=library.games();
  require(games.size()==2 && games[0].runner=="steam" && games[0].executable.empty(),"runner manifests retain an absent executable for discovery");
  require(std::set<fs::path>{games[0].content,games[1].content}==std::set<fs::path>{"100","200"},"Steam app ids are identities, not relative filesystem paths");
  require(library.scan_games()==0 && library.games().size()==2,"runner discovery is idempotent");
}
void arguments() {
  const std::vector<std::string> values{"", "a path with spaces", "literal;$(value)", "a\\b\"c", "é"};
  require(parse_arguments(format_arguments(values)) == values, "argv formatting roundtrips empty, quoted, UTF-8, metacharacter and slash values");
  bool rejected = false; try { (void)parse_arguments("\"unclosed"); } catch (const std::runtime_error &) { rejected = true; }
  require(rejected, "malformed argument quotes rejected");
}
}
int main() {
  const auto root = fs::temp_directory_path() / ("openxmb-desktop-library-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    fs::create_directories(root);
    profiles(root / "profiles");
    games(root / "games");
    arguments();
    runner_manifests(root / "runner-manifests");
    fs::remove_all(root);
    std::cout << "Desktop profiles, game launch, custom sections and local social checks passed\n";
  } catch (const std::exception &error) {
    fs::remove_all(root);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
