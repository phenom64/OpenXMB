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

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openxmb::desktop {
struct Profile {
  std::string id, name;
  std::filesystem::path avatar;
};
struct Game {
  std::string id, title, description, genre, developer, category;
  std::string platform, title_id;
  std::filesystem::path executable, working_directory, icon, background, preview, music;
  std::filesystem::path game_directory;
  std::vector<std::string> arguments;
  int year{};
  bool favorite{};
  std::uint64_t play_count{}, last_played{};
};
struct CustomSection {
  std::string id, title;
  std::filesystem::path folder, icon;
};
struct Friend {
  std::string id, name, status;
  bool present{}; // Local simulation only; no remote presence is represented.
};
struct Trophy {
  std::string id, title, game, description, grade{"bronze"};
  bool unlocked{};
  std::uint64_t unlocked_at{};
};
struct Account {
  std::string display_name, biography, status{"Available locally"};
  std::filesystem::path avatar;
};

std::filesystem::path default_data_root();
std::vector<std::string> parse_arguments(const std::string &text);
std::string format_arguments(const std::vector<std::string> &arguments);
std::map<std::string, std::string> read_param_sfo(const std::filesystem::path &path);
std::optional<std::filesystem::path> rpcs3_executable();
std::vector<std::filesystem::path> rpcs3_game_roots();
Game import_rpcs3_game(const std::filesystem::path &directory);
// Starts an explicit argv vector without a command shell. Returns only after
// the operating system accepts the executable; the child is reaped separately.
void launch_game(const Game &game);

class Library {
public:
  explicit Library(std::filesystem::path root = default_data_root());
  const std::filesystem::path &root() const noexcept { return root_; }
  std::vector<Profile> profiles() const;
  Profile active_profile() const;
  Profile create_profile(std::string name, const std::filesystem::path &baseline_config = {});
  void update_profile(Profile profile);
  void remove_profile(const std::string &id);
  std::filesystem::path switch_profile(const std::string &id);
  std::filesystem::path profile_directory(const std::string &id) const;
  std::filesystem::path active_directory() const;
  std::filesystem::path config_path(const std::string &id) const;
  std::filesystem::path themes_path(const std::string &id) const;

  std::vector<Game> games() const;
  Game save_game(Game game);
  void remove_game(const std::string &id);
  void mark_played(const std::string &id);
  std::vector<std::filesystem::path> game_roots() const;
  void add_game_root(const std::filesystem::path &path);
  void remove_game_root(const std::filesystem::path &path);
  std::size_t scan_games();
  // Optional RAWG artwork lookup, invoked only by an explicit UI action. The
  // API key is read transiently from the named process variable, never saved.
  void set_artwork_provider(bool enabled, std::string key_variable = "RAWG_API_KEY");
  bool artwork_provider_available() const;
  std::filesystem::path fetch_artwork(const std::string &game_id);

  std::vector<CustomSection> custom_sections() const;
  CustomSection save_section(CustomSection section);
  void remove_section(const std::string &id);
  std::vector<Friend> friends() const;
  Friend save_friend(Friend person);
  void remove_friend(const std::string &id);
  Account account() const;
  void save_account(Account account);
  std::vector<Trophy> trophies() const;
  Trophy save_trophy(Trophy trophy);
  void remove_trophy(const std::string &id);

private:
  std::filesystem::path root_;
};
} // namespace openxmb::desktop
