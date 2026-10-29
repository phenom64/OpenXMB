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
#include "openxmb/desktop/runners.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
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
  require(library.scan_games({false,false}) == 1 && library.scan_games({false,false}) == 0, "scan imports one game and repeat scan deduplicates");
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
  require(library.scan_games({false,false})==2,"runner manifests without an executable remain distinct");
  const auto games=library.games();
  require(games.size()==2 && games[0].runner=="steam" && games[0].executable.empty(),"runner manifests retain an absent executable for discovery");
  require(std::set<fs::path>{games[0].content,games[1].content}==std::set<fs::path>{"100","200"},"Steam app ids are identities, not relative filesystem paths");
  require(library.scan_games({false,false})==0 && library.games().size()==2,"runner discovery is idempotent");
  auto original=library.games().front();
  original.icon=folder/"manual-icon.png";library.save_game(original);
  auto refreshed=original;refreshed.icon=folder/"automatic-header.jpg";refreshed.background=folder/"hero-a.jpg";
  library.import_games({{refreshed,true}});
  auto saved=library.games().front();
  require(saved.icon==original.icon && saved.background==refreshed.background,"refresh preserves a manual image and fills missing native roles");
  refreshed.background=folder/"hero-b.jpg";library.import_games({{refreshed,true}});
  saved=library.games().front();
  require(saved.icon==original.icon && saved.background==refreshed.background,"automatic images refresh independently of manual overrides");
  refreshed.background.clear();library.import_games({{refreshed,true}});
  require(library.games().front().background==saved.background,"rescanning a source without a role must retain cached automatic artwork");
}
void arguments() {
  const std::vector<std::string> values{"", "a path with spaces", "literal;$(value)", "a\\b\"c", "é"};
  require(parse_arguments(format_arguments(values)) == values, "argv formatting roundtrips empty, quoted, UTF-8, metacharacter and slash values");
  bool rejected = false; try { (void)parse_arguments("\"unclosed"); } catch (const std::runtime_error &) { rejected = true; }
  require(rejected, "malformed argument quotes rejected");
}
void steam_launch_settings(const fs::path& root,const fs::path& test_executable) {
  Library library(root);
  Game source; source.title="Steam fixture"; source.runner="steam"; source.content="12345";
  require(library.import_games({{source,true}})==1,"Steam fixture imported");
  const auto original=library.games().front();
  const auto metadata=library.active_directory()/"library.json";
  nlohmann::json state; std::ifstream(metadata)>>state;
  const nlohmann::json override{{"mode","direct"},{"executable","C:/fixture game/game.exe"},
    {"arguments",std::vector<std::string>{"literal ; $(value)","spaced value",""}},{"working_directory","C:/fixture game"}};
  state["games"][0]["launch_override"]=override;
  {std::ofstream output(metadata);output<<state;}
  library.save_game(library.games().front());
  {std::ifstream input(metadata);input>>state;}
  require(state["games"][0].value("launch_override",nlohmann::json::object())==override,"Steam direct launch settings survive reopening and saving");
  source.title="Refreshed Steam fixture";source.executable="steam.exe";
  require(library.import_games({{source,true}})==0,"Steam launch override does not create duplicate on discovery refresh");
  {std::ifstream input(metadata);input>>state;}
  require(state["games"][0]["launch_override"]==override,"discovery preserves every saved direct launch setting");
  const auto refreshed=library.games().front();
  require(refreshed.id==original.id && refreshed.runner=="steam" && refreshed.content=="12345" && refreshed.title==source.title,
    "Steam provider identity and refreshed metadata remain independent of direct launch settings");
  Game direct=refreshed;
  direct.launch_override.executable=test_executable;
  const auto folder=root/"literal launch folder";fs::create_directories(folder);
  const std::string payload="literal ; $(touch injected) & \"quoted\" value";
  direct.launch_override.arguments={"--direct-launch-child","launch-result.txt",payload};
  direct.launch_override.working_directory=folder;
  direct.favorite=true;library.save_game(direct);library.mark_played(direct.id);
  require(library.import_games({{source,true}})==0&&library.games().size()==1,"Steam refresh after saving direct launch never duplicates the entry");
  direct=library.games().front();
  require(direct.favorite&&direct.play_count==1&&direct.launch_override.mode==GameLaunchMode::direct,"Steam launch overrides retain favorites and history");
  const auto target=launch_game_target(direct);
  require(target.runner=="native"&&target.executable==test_executable&&target.arguments==direct.launch_override.arguments&&target.working_directory==folder,
    "Steam direct mode resolves to the explicit native executable, literal argv and cwd");
  require(direct.runner=="steam"&&direct.content=="12345"&&target.launch_override.mode==GameLaunchMode::provider,"resolving direct launch leaves stored Steam identity intact");
  RunnerDiscoveryOptions options;options.search_path=false;options.search_defaults=false;
  const auto plan=plan_game_launch(target,options);
  require(plan&&plan.runner=="native"&&plan.arguments.size()==4&&plan.arguments[3]==payload&&plan.working_directory==folder&&plan.tracking==TrackingQuality::direct_child,
    "real native runner plan launches direct executable without resolving Steam or changing literal arguments");
  launch_game(direct);
  const auto output=folder/"launch-result.txt";
  for(int retry=0;retry<200&&!fs::exists(output);++retry)std::this_thread::sleep_for(std::chrono::milliseconds(5));
  std::ifstream input(output);std::string value;std::getline(input,value);
  require(value==payload&&!fs::exists(folder/"injected"),"real direct child receives literal arguments and selected working folder");
  direct.launch_override.working_directory.clear();
  require(launch_game_target(direct).working_directory==test_executable.parent_path(),"direct working folder defaults to the executable's folder");
  auto malformed=direct;
  auto invalid_path=test_executable.native();invalid_path.push_back(fs::path::value_type{});
  invalid_path+=fs::path("unexpected.exe").native();malformed.launch_override.executable=fs::path(invalid_path);
  bool malformed_rejected=false;try{(void)launch_game_target(malformed);}catch(const std::runtime_error&){malformed_rejected=true;}
  require(malformed_rejected,"direct executable paths cannot contain embedded NUL even when their prefix exists");
  direct.launch_override.executable=root/"missing-game.exe";
  bool rejected=false;try{(void)launch_game_target(direct);}catch(const std::runtime_error&){rejected=true;}
  require(rejected,"missing direct executable produces an actionable failure instead of invoking Steam");
  direct.launch_override.mode=GameLaunchMode::provider;library.save_game(direct);
  const auto restored=launch_game_target(Library(root).games().front());
  require(restored.runner=="steam"&&restored.content=="12345"&&restored.executable==source.executable,
    "switching to default provider mode restores Steam despite a missing remembered direct executable");
  require(restored.launch_override.executable==direct.launch_override.executable,"default provider mode remembers direct launch configuration for later");
  Game legacy;legacy.title="Legacy Steam entry";legacy.runner="steam";legacy.content="22222";
  legacy=library.save_game(legacy);
  nlohmann::json legacy_state;{std::ifstream file(metadata);file>>legacy_state;}
  for(auto& item:legacy_state["games"])item.erase("launch_override");
  {std::ofstream file(metadata);file<<legacy_state;}
  for(const auto& entry:Library(root).games())require(entry.launch_override.mode==GameLaunchMode::provider,"legacy libraries without launch settings migrate to provider mode");
}
void support_visibility(const fs::path& root,const fs::path& test_executable) {
  Library library(root);
  Game support;support.title="Steamworks Common Redistributables";support.runner="steam";support.content="228980";
  support.favorite=true;support.play_count=7;support.icon=root/"custom support icon.png";
  support.automatic_artwork["icon"]=root/"old automatic icon.png";
  support.launch_override.mode=GameLaunchMode::direct;support.launch_override.executable=test_executable;
  support.launch_override.arguments={"literal saved support override"};
  const auto first=library.save_game(support);
  support.title="Ubisoft Connect Services";support.content="4099820";support.id.clear();
  const auto second=library.save_game(support);
  Game native;native.title="Ubisoft Connect Services";native.runner="native";native.executable=test_executable;native.icon=root/"native manual artwork.png";
  native=library.save_game(native);
  Game real;real.title="Trackmania";real.runner="steam";real.content="2225070";real.icon=root/"real manual artwork.png";
  real=library.save_game(real);
  const auto metadata=library.active_directory()/"library.json";
  nlohmann::json before;{std::ifstream input(metadata);input>>before;}
  // A future/private field must also survive operations performed through the
  // filtered view; reconstructing hidden records through Game would lose it.
  for(auto& item:before["games"])if(item["id"]==first.id||item["id"]==second.id)item["retained_user_note"]="do not rewrite this hidden record";
  {std::ofstream output(metadata);output<<before;}
  const auto saved_record=[&](const nlohmann::json& state,const std::string& id) {
    for(const auto& item:state.at("games"))if(item.at("id")==id)return item.dump();
    throw std::runtime_error("stored support record disappeared");
  };
  const auto first_bytes=saved_record(before,first.id),second_bytes=saved_record(before,second.id);
  const auto visible=library.games();
  require(visible.size()==2,"stored support products are hidden without hiding real or explicit native games");
  require(std::any_of(visible.begin(),visible.end(),[&](const Game& value){return value.id==native.id&&value.icon==native.icon;}),"explicit Ubisoft-named native imports retain their artwork and visibility");
  real.favorite=true;real.launch_override.mode=GameLaunchMode::direct;real.launch_override.executable=test_executable;
  real.launch_override.arguments={"saved real game override"};library.save_game(real);library.mark_played(real.id);
  auto refreshed=real;refreshed.title="Trackmania refreshed";refreshed.icon=root/"new automatic header.png";refreshed.launch_override={};
  require(library.import_games({{refreshed,true},{first,true},{second,true}})==0,"rescanning support products never adds them back or duplicates the real game");
  nlohmann::json after;{std::ifstream input(metadata);input>>after;}
  require(after["games"].size()==4&&saved_record(after,first.id)==first_bytes&&saved_record(after,second.id)==second_bytes,"favourite history override and discovery updates preserve byte-equivalent hidden support records");
  const auto updated=library.games();
  const auto found=std::find_if(updated.begin(),updated.end(),[&](const Game& value){return value.id==real.id;});
  require(found!=updated.end()&&found->favorite&&found->play_count==1&&found->icon==real.icon&&found->launch_override.arguments==real.launch_override.arguments,"visible real game mutations preserve manual artwork and launch settings");
}
}
int main(int argc,char** argv) {
  if(argc==4&&std::string(argv[1])=="--direct-launch-child") {std::ofstream output(argv[2]);output<<argv[3];return output?0:2;}
  const auto root = fs::temp_directory_path() / ("openxmb-desktop-library-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    fs::create_directories(root);
    profiles(root / "profiles");
    games(root / "games");
    arguments();
    runner_manifests(root / "runner-manifests");
    steam_launch_settings(root / "steam-launch-settings",fs::absolute(argv[0]));
    support_visibility(root / "support-visibility",fs::absolute(argv[0]));
    fs::remove_all(root);
    std::cout << "Desktop profiles, game launch, custom sections and local social checks passed\n";
  } catch (const std::exception &error) {
    fs::remove_all(root);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
