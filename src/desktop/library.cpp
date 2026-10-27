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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

#ifdef OPENXMB_HAVE_CURL
#include <curl/curl.h>
#endif
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace openxmb::desktop {
namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;
std::atomic<std::uint64_t> serial{};
std::uint64_t now() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}
std::string identifier(const char *prefix) {
  return std::string(prefix) + '-' + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + '-' + std::to_string(++serial);
}
void valid_id(const std::string &id) {
  if (id.empty() || id.size() > 128 || !std::ranges::all_of(id, [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; }))
    throw std::runtime_error("Invalid local library identifier");
}
void valid_name(const std::string &name) {
  if (name.empty() || name.size() > 512 || name.find('\0') != std::string::npos) throw std::runtime_error("Enter a name between 1 and 512 bytes");
}
json read(const fs::path &file, json fallback = json::object()) {
  if (!fs::exists(file)) return fallback;
  if (!fs::is_regular_file(file) || fs::file_size(file) > 8 * 1024 * 1024) throw std::runtime_error("Local library metadata is invalid or exceeds 8 MiB");
  try {
    std::ifstream input(file, std::ios::binary);
    auto result = json::parse(input);
    if (!result.is_object()) throw std::runtime_error("Local library metadata must be a JSON object");
    return result;
  } catch (const json::exception &error) { throw std::runtime_error("Cannot read local library metadata: " + std::string(error.what())); }
}
void write(const fs::path &file, const json &value) {
  fs::create_directories(file.parent_path());
  const auto temporary = file.parent_path() / (file.filename().string() + '.' + identifier("tmp"));
  try {
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output) throw std::runtime_error("Cannot create local library save file");
      output << value.dump(2) << '\n';
      output.flush();
      if (!output) throw std::runtime_error("Local library save did not complete");
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot replace local library save file");
#else
    const int fd = ::open(temporary.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("Cannot reopen local library save file");
    const int synced = ::fsync(fd);
    ::close(fd);
    if (synced != 0) throw std::runtime_error("Cannot flush local library save file");
    fs::rename(temporary, file);
    const int directory = ::open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (directory >= 0) { ::fsync(directory); ::close(directory); }
#endif
  } catch (...) { std::error_code ignored; fs::remove(temporary, ignored); throw; }
}
json profile_json(const Profile &p) { return {{"id", p.id}, {"name", p.name}, {"avatar", p.avatar.string()}}; }
Profile profile(const json &p) { Profile result{p.at("id").get<std::string>(), p.at("name").get<std::string>(), p.value("avatar", "")}; valid_id(result.id); valid_name(result.name); return result; }
json global(const fs::path &root) { return read(root / "profiles.json"); }
std::string active_id(const fs::path &root) {
  const auto state = global(root);
  const auto id = state.at("active_profile").get<std::string>();
  valid_id(id);
  return id;
}
json library(const fs::path &root) {
  return read(root / "profiles" / active_id(root) / "library.json", {{"version", 1}});
}
void save_library(const fs::path &root, const json &state) {
  write(root / "profiles" / active_id(root) / "library.json", state);
}
template<typename T, typename Decode>
std::vector<T> items(const json &state, const char *key, Decode decode) {
  std::vector<T> result;
  if (!state.contains(key)) return result;
  const auto &values = state.at(key);
  if (!values.is_array() || values.size() > 10000) throw std::runtime_error("Invalid local library collection");
  for (const auto &value : values) result.push_back(decode(value));
  return result;
}
void upsert(json &state, const char *key, json value) {
  auto &array = state[key];
  if (array.is_null()) array = json::array();
  if (!array.is_array()) throw std::runtime_error("Invalid local library collection");
  const auto id = value.at("id").get<std::string>();
  valid_id(id);
  for (auto &existing : array) {
    if (existing.at("id") == id) { existing = std::move(value); return; }
  }
  if (array.size() >= 10000) throw std::runtime_error("Local library exceeds 10000 entries");
  array.push_back(std::move(value));
}
void erase(const fs::path &root, const char *key, const std::string &id) {
  valid_id(id);
  auto state = library(root);
  auto &array = state[key];
  if (!array.is_array()) return;
  std::erase_if(array.get_ref<json::array_t &>(), [&](const json &value) { return value.at("id") == id; });
  save_library(root, state);
}
json game_json(const Game &g) {
  return {{"id", g.id}, {"title", g.title}, {"description", g.description}, {"genre", g.genre}, {"developer", g.developer}, {"category", g.category},
    {"executable", g.executable.string()}, {"arguments", g.arguments}, {"working_directory", g.working_directory.string()}, {"icon", g.icon.string()},
    {"background", g.background.string()}, {"preview", g.preview.string()}, {"music", g.music.string()}, {"year", g.year}, {"favorite", g.favorite},
    {"play_count", g.play_count}, {"last_played", g.last_played}, {"platform", g.platform}, {"title_id", g.title_id}, {"game_directory", g.game_directory.string()}};
}
Game game(const json &value) {
  Game g;
  g.id = value.value("id", ""); g.title = value.value("title", ""); g.description = value.value("description", "");
  g.genre = value.value("genre", ""); g.developer = value.value("developer", ""); g.category = value.value("category", "");
  g.executable = value.value("executable", ""); g.arguments = value.value("arguments", std::vector<std::string>{});
  g.working_directory = value.value("working_directory", ""); g.icon = value.value("icon", ""); g.background = value.value("background", "");
  g.preview = value.value("preview", ""); g.music = value.value("music", ""); g.year = value.value("year", 0); g.favorite = value.value("favorite", false);
  g.play_count = value.value("play_count", std::uint64_t{}); g.last_played = value.value("last_played", std::uint64_t{});
  g.platform = value.value("platform", ""); g.title_id = value.value("title_id", ""); g.game_directory = value.value("game_directory", "");
  return g;
}
std::string lower(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }); return value;
}
bool executable_file(const fs::directory_entry &entry) {
  if (!entry.is_regular_file() || entry.is_symlink()) return false;
#ifdef _WIN32
  return lower(entry.path().extension().string()) == ".exe";
#else
  const auto permissions = entry.status().permissions();
  const bool executable = (permissions & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) != fs::perms::none;
  const auto ext = lower(entry.path().extension().string());
  return executable && ext != ".so" && ext != ".desktop" && ext != ".py";
#endif
}
Game desktop_file(const fs::path &path) {
  std::ifstream stream(path);
  std::map<std::string, std::string> fields;
  std::string line;
  bool group = false;
  while (std::getline(stream, line)) {
    if (line == "[Desktop Entry]") { group = true; continue; }
    if (!line.empty() && line[0] == '[') { group = false; continue; }
    const auto equal = line.find('=');
    if (group && equal != std::string::npos) fields[line.substr(0, equal)] = line.substr(equal + 1);
  }
  if (fields["Type"] != "Application" || fields["Hidden"] == "true" || fields["NoDisplay"] == "true") throw std::runtime_error("Not a launchable desktop entry");
  auto arguments = parse_arguments(fields["Exec"]);
  std::erase_if(arguments, [](const std::string &argument) { return argument == "%f" || argument == "%F" || argument == "%u" || argument == "%U" || argument == "%i" || argument == "%c" || argument == "%k"; });
  if (arguments.empty()) throw std::runtime_error("Desktop entry has no executable");
  Game result;
  result.title = fields["Name"].empty() ? path.stem().string() : fields["Name"];
  result.executable = arguments.front(); arguments.erase(arguments.begin()); result.arguments = std::move(arguments);
  result.description = fields["Comment"]; result.working_directory = fields["Path"];
  if (fs::path(fields["Icon"]).is_absolute()) result.icon = fields["Icon"];
  return result;
}
} // namespace

fs::path default_data_root() {
  if (const auto *root = std::getenv("OPENXMB_DATA_DIR"); root && *root) return fs::path(root);
  if (const auto *root = std::getenv("XDG_DATA_HOME"); root && *root) return fs::path(root) / "OpenXMB";
#ifdef _WIN32
  if (const auto *root = std::getenv("LOCALAPPDATA"); root && *root) return fs::path(root) / "OpenXMB";
#endif
  if (const auto *root = std::getenv("HOME"); root && *root) return fs::path(root) / ".local/share/OpenXMB";
  throw std::runtime_error("Set OPENXMB_DATA_DIR to a writable local data directory");
}
std::vector<std::string> parse_arguments(const std::string &text) {
  std::vector<std::string> result;
  std::string token;
  char quote = 0;
  bool escape = false, started = false;
  for (char character : text) {
    if (character == '\0') throw std::runtime_error("Arguments cannot contain NUL");
    if (escape) { token += character; escape = false; started = true; }
    else if (character == '\\' && quote != '\'') { escape = true; started = true; }
    else if (quote) { if (character == quote) quote = 0; else token += character; started = true; }
    else if (character == '\'' || character == '"') { quote = character; started = true; }
    else if (std::isspace(static_cast<unsigned char>(character))) { if (started) { result.push_back(std::move(token)); token.clear(); started = false; } }
    else { token += character; started = true; }
  }
  if (quote || escape) throw std::runtime_error("Close the quote or escaped character in arguments");
  if (started) result.push_back(std::move(token));
  if (result.size() > 256) throw std::runtime_error("Too many launch arguments");
  return result;
}
std::string format_arguments(const std::vector<std::string> &arguments) {
  std::string result;
  for (const auto &argument : arguments) {
    if (!result.empty()) result += ' ';
    result += '"';
    for (char character : argument) { if (character == '\\' || character == '"') result += '\\'; result += character; }
    result += '"';
  }
  return result;
}
std::map<std::string, std::string> read_param_sfo(const fs::path &path) {
  if (!fs::is_regular_file(path) || fs::file_size(path) < 20 || fs::file_size(path) > 1024 * 1024)
    throw std::runtime_error("PARAM.SFO is missing, truncated, or too large");
  std::ifstream input(path, std::ios::binary);
  const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
  if (bytes.size() < 20 || std::memcmp(bytes.data(), "\0PSF", 4) != 0) throw std::runtime_error("PARAM.SFO has an invalid header");
  const auto bounds = [&](std::size_t offset, std::size_t size) {
    if (offset > bytes.size() || size > bytes.size() - offset) throw std::runtime_error("PARAM.SFO entry is outside the file");
  };
  const auto u16 = [&](std::size_t offset) { bounds(offset, 2); return static_cast<unsigned>(bytes[offset]) | (static_cast<unsigned>(bytes[offset + 1]) << 8); };
  const auto u32 = [&](std::size_t offset) {
    bounds(offset, 4); return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
      (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
  };
  const std::size_t keys = u32(8), data = u32(12), count = u32(16);
  if (count > 1024 || keys < 20 || data < keys || data > bytes.size()) throw std::runtime_error("PARAM.SFO has invalid table offsets");
  bounds(20, count * 16);
  if (20 + count * 16 > keys) throw std::runtime_error("PARAM.SFO entry table overlaps its keys");
  std::map<std::string, std::string> result;
  for (std::size_t index = 0; index < count; ++index) {
    const auto entry = 20 + index * 16;
    const std::size_t key_offset = u16(entry), format = u16(entry + 2), length = u32(entry + 4), maximum = u32(entry + 8), value_offset = u32(entry + 12);
    if (key_offset >= data - keys || length > maximum || value_offset > bytes.size() - data) throw std::runtime_error("PARAM.SFO has an invalid entry");
    bounds(data + value_offset, length);
    const auto key_begin = bytes.begin() + static_cast<std::ptrdiff_t>(keys + key_offset);
    const auto key_end = std::find(key_begin, bytes.begin() + static_cast<std::ptrdiff_t>(data), 0);
    if (key_end == bytes.begin() + static_cast<std::ptrdiff_t>(data) || key_end - key_begin > 128) throw std::runtime_error("PARAM.SFO key is not terminated");
    const std::string key(key_begin, key_end);
    if (format == 0x0204 || format == 0x0004) {
      if (length > 65536) throw std::runtime_error("PARAM.SFO string is too large");
      const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(data + value_offset);
      const auto end = std::find(begin, begin + static_cast<std::ptrdiff_t>(length), 0);
      result[key] = std::string(begin, end);
    } else if (format == 0x0404 && length == 4) result[key] = std::to_string(u32(data + value_offset));
  }
  return result;
}
std::optional<fs::path> rpcs3_executable() {
  const auto usable = [](const fs::path &path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error)) return false;
#ifdef _WIN32
    return true;
#else
    return ::access(path.c_str(), X_OK) == 0;
#endif
  };
  if (const auto *configured = std::getenv("RPCS3_EXECUTABLE"); configured && *configured && usable(configured)) return fs::absolute(configured);
  const auto *search = std::getenv("PATH");
  if (!search) return {};
#ifdef _WIN32
  constexpr char separator = ';';
  constexpr const char *names[]{"rpcs3.exe", "RPCS3.exe"};
#else
  constexpr char separator = ':';
  constexpr const char *names[]{"rpcs3", "rpcs3.AppImage"};
#endif
  std::string value(search);
  for (std::size_t start = 0; start <= value.size();) {
    const auto end = value.find(separator, start);
    const auto part = value.substr(start, end == std::string::npos ? end : end - start);
    if (!part.empty()) for (const auto *name : names) if (const auto path = fs::path(part) / name; usable(path)) return fs::absolute(path);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return {};
}
std::vector<fs::path> rpcs3_game_roots() {
  std::vector<fs::path> candidates;
  if (const auto *configured = std::getenv("RPCS3_DATA_DIR"); configured && *configured) candidates.emplace_back(fs::path(configured) / "dev_hdd0/game");
  if (const auto *configured = std::getenv("XDG_CONFIG_HOME"); configured && *configured) candidates.emplace_back(fs::path(configured) / "rpcs3/dev_hdd0/game");
  if (const auto *home = std::getenv("HOME"); home && *home) {
    candidates.emplace_back(fs::path(home) / ".config/rpcs3/dev_hdd0/game");
    candidates.emplace_back(fs::path(home) / "Library/Application Support/rpcs3/dev_hdd0/game");
    candidates.emplace_back(fs::path(home) / ".var/app/net.rpcs3.RPCS3/config/rpcs3/dev_hdd0/game");
  }
  if (const auto *appdata = std::getenv("APPDATA"); appdata && *appdata) candidates.emplace_back(fs::path(appdata) / "rpcs3/dev_hdd0/game");
  if (const auto executable = rpcs3_executable()) candidates.emplace_back(executable->parent_path() / "dev_hdd0/game");
  std::vector<fs::path> result;
  for (const auto &candidate : candidates) {
    std::error_code error;
    if (!fs::is_directory(candidate, error)) continue;
    const auto normalized = fs::weakly_canonical(candidate, error);
    if (!error && std::ranges::find(result, normalized) == result.end()) result.push_back(normalized);
  }
  return result;
}
Game import_rpcs3_game(const fs::path &directory) {
  auto folder = fs::absolute(directory).lexically_normal();
  if (folder.filename() == "PARAM.SFO") folder = folder.parent_path();
  if (!fs::is_regular_file(folder / "PARAM.SFO") && fs::is_regular_file(folder / "PS3_GAME/PARAM.SFO")) folder /= "PS3_GAME";
  const auto metadata = read_param_sfo(folder / "PARAM.SFO");
  const auto value = [&](const char *key) { const auto found = metadata.find(key); return found == metadata.end() ? std::string{} : found->second; };
  Game game;
  game.platform = "PlayStation 3 (RPCS3)";
  game.title = value("TITLE").empty() ? directory.filename().string() : value("TITLE");
  game.title_id = value("TITLE_ID");
  game.game_directory = folder;
  game.description = game.platform + "  " + game.title_id + (value("APP_VER").empty() ? "" : "  v" + value("APP_VER"));
  game.category = "PlayStation 3";
  game.executable = rpcs3_executable().value_or(fs::path("rpcs3"));
  const auto boot = folder / "USRDIR/EBOOT.BIN";
  game.arguments = {"--no-gui", fs::is_regular_file(boot) ? boot.string() : folder.string()};
  game.working_directory = folder;
  for (const auto &[name, target] : std::initializer_list<std::pair<const char *, fs::path *>>{{"ICON0.PNG", &game.icon}, {"PIC1.PNG", &game.background}, {"ICON1.PAM", &game.preview}, {"SND0.AT3", &game.music}})
    if (fs::is_regular_file(folder / name)) *target = folder / name;
  if (game.background.empty() && fs::is_regular_file(folder / "PIC0.PNG")) game.background = folder / "PIC0.PNG";
  return game;
}
void launch_game(const Game &game) {
  if (game.executable.empty()) throw std::runtime_error("Set this game's executable before launching");
  std::vector<std::string> arguments{game.executable.string()};
  arguments.insert(arguments.end(), game.arguments.begin(), game.arguments.end());
  for (const auto &argument : arguments) if (argument.find('\0') != std::string::npos) throw std::runtime_error("Launch arguments contain NUL");
#ifdef _WIN32
  auto wide = [](const std::string &input) { const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.c_str(), -1, nullptr, 0); if (!size) throw std::runtime_error("Invalid UTF-8 launch argument"); std::wstring result(size, 0); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.c_str(), -1, result.data(), size); result.pop_back(); return result; };
  std::wstring command;
  for (const auto &argument : arguments) {
    if (!command.empty()) command += L' ';
    command += L'"'; std::size_t slashes = 0;
    for (wchar_t c : wide(argument)) {
      if (c == L'\\') { ++slashes; continue; }
      command.append(slashes * (c == L'"' ? 2 : 1) + (c == L'"' ? 1 : 0), L'\\'); slashes = 0; command += c;
    }
    command.append(slashes * 2, L'\\'); command += L'"';
  }
  STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, game.working_directory.empty() ? nullptr : game.working_directory.c_str(), &startup, &process)) throw std::runtime_error("Windows could not start the selected game");
  CloseHandle(process.hThread); CloseHandle(process.hProcess);
#else
  std::vector<char *> argv;
  for (auto &argument : arguments) argv.push_back(argument.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  int result = posix_spawn_file_actions_init(&actions);
  if (result) throw std::runtime_error(std::strerror(result));
  if (!game.working_directory.empty()) result = posix_spawn_file_actions_addchdir_np(&actions, game.working_directory.c_str());
  pid_t pid{};
  if (!result) result = posix_spawnp(&pid, arguments.front().c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (result) throw std::runtime_error("Cannot launch game: " + std::string(std::strerror(result)));
  std::thread([pid] { int status; while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {} }).detach();
#endif
}

Library::Library(fs::path root) : root_(fs::absolute(std::move(root))) {
  fs::create_directories(root_);
  if (!fs::exists(root_ / "profiles.json")) {
    Profile first{"default", "Default", {}};
    fs::create_directories(themes_path(first.id));
    write(config_path(first.id), {{"shell", json::object()}});
    write(root_ / "profiles.json", {{"version", 1}, {"active_profile", first.id}, {"profiles", json::array({profile_json(first)})}});
  }
  const auto state = global(root_);
  if (state.value("version", 0) != 1) throw std::runtime_error("Unsupported local profile library version");
  const auto entries = profiles();
  const auto active = active_id(root_);
  if (std::ranges::none_of(entries, [&](const Profile &p) { return p.id == active; })) throw std::runtime_error("Active profile is missing from profile library");
}
std::vector<Profile> Library::profiles() const { return items<Profile>(global(root_), "profiles", profile); }
Profile Library::active_profile() const {
  const auto id = active_id(root_);
  for (const auto &item : profiles()) if (item.id == id) return item;
  throw std::runtime_error("Active profile does not exist");
}
fs::path Library::profile_directory(const std::string &id) const { valid_id(id); return root_ / "profiles" / id; }
fs::path Library::active_directory() const { return profile_directory(active_id(root_)); }
fs::path Library::config_path(const std::string &id) const { return profile_directory(id) / "config.json"; }
fs::path Library::themes_path(const std::string &id) const { return profile_directory(id) / "themes"; }
Profile Library::create_profile(std::string name, const fs::path &baseline_config) {
  valid_name(name);
  Profile result{identifier("profile"), std::move(name), {}};
  fs::create_directories(themes_path(result.id));
  const auto config = baseline_config.empty() ? json{{"shell", json::object()}} : read(baseline_config);
  write(config_path(result.id), config);
  auto state = global(root_); upsert(state, "profiles", profile_json(result)); write(root_ / "profiles.json", state);
  return result;
}
void Library::update_profile(Profile p) {
  valid_id(p.id); valid_name(p.name);
  const auto entries = profiles();
  if (std::ranges::none_of(entries, [&](const Profile &existing) { return existing.id == p.id; })) throw std::runtime_error("Profile does not exist");
  auto state = global(root_); upsert(state, "profiles", profile_json(p)); write(root_ / "profiles.json", state);
}
void Library::remove_profile(const std::string &id) {
  valid_id(id);
  auto state = global(root_);
  auto &entries = state.at("profiles").get_ref<json::array_t &>();
  if (entries.size() <= 1) throw std::runtime_error("Keep at least one local profile");
  if (std::ranges::none_of(entries, [&](const json &value) { return value.at("id") == id; })) throw std::runtime_error("Profile does not exist");
  std::erase_if(entries, [&](const json &value) { return value.at("id") == id; });
  if (state.at("active_profile") == id) state["active_profile"] = entries.front().at("id");
  fs::path archived;
  if (fs::exists(profile_directory(id))) {
    fs::create_directories(root_ / "profile-trash");
    archived = root_ / "profile-trash" / (id + '-' + identifier("deleted"));
    fs::rename(profile_directory(id), archived);
  }
  try { write(root_ / "profiles.json", state); }
  catch (...) { if (!archived.empty()) fs::rename(archived, profile_directory(id)); throw; }
}
fs::path Library::switch_profile(const std::string &id) {
  valid_id(id);
  const auto entries = profiles();
  if (std::ranges::none_of(entries, [&](const Profile &p) { return p.id == id; })) throw std::runtime_error("Profile does not exist");
  auto state = global(root_); state["active_profile"] = id; write(root_ / "profiles.json", state);
  return config_path(id);
}
std::vector<Game> Library::games() const { return items<Game>(library(root_), "games", game); }
Game Library::save_game(Game value) {
  valid_name(value.title);
  if (value.id.empty()) value.id = identifier("game");
  valid_id(value.id);
  if (value.arguments.size() > 256) throw std::runtime_error("Too many launch arguments");
  auto state = library(root_); upsert(state, "games", game_json(value)); save_library(root_, state); return value;
}
void Library::remove_game(const std::string &id) { erase(root_, "games", id); }
void Library::mark_played(const std::string &id) {
  for (auto value : games()) if (value.id == id) { ++value.play_count; value.last_played = now(); save_game(std::move(value)); return; }
  throw std::runtime_error("Game does not exist");
}
std::vector<fs::path> Library::game_roots() const {
  std::vector<fs::path> result;
  for (const auto &path : library(root_).value("game_roots", std::vector<std::string>{})) result.emplace_back(path);
  return result;
}
void Library::add_game_root(const fs::path &path) {
  if (!fs::is_directory(path)) throw std::runtime_error("Game root must be an existing directory");
  auto state = library(root_); auto roots = state.value("game_roots", std::vector<std::string>{});
  const auto value = fs::weakly_canonical(path).string();
  if (std::ranges::find(roots, value) == roots.end()) roots.push_back(value);
  state["game_roots"] = roots; save_library(root_, state);
}
void Library::remove_game_root(const fs::path &path) {
  auto state = library(root_); auto roots = state.value("game_roots", std::vector<std::string>{});
  const auto value = fs::weakly_canonical(path).string(); std::erase(roots, value); state["game_roots"] = roots; save_library(root_, state);
}
std::size_t Library::scan_games() {
  auto existing = games();
  std::set<std::string> paths;
  for (const auto &g : existing) paths.insert(g.executable.string() + '\n' + format_arguments(g.arguments));
  std::size_t count = 0, examined = 0;
  for (const auto &root : game_roots()) {
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
      if (++examined > 10000) throw std::runtime_error("Game scan exceeded 10000 entries; choose narrower roots");
      if (it.depth() > 5) { it.disable_recursion_pending(); continue; }
      if (it->is_symlink()) { it.disable_recursion_pending(); continue; }
      Game found;
      const auto path = it->path();
      try {
        if (it->is_directory() && (fs::is_regular_file(path / "PARAM.SFO") || fs::is_regular_file(path / "PS3_GAME/PARAM.SFO"))) {
          found = import_rpcs3_game(path);
          it.disable_recursion_pending();
        } else if (path.filename() == "PARAM.SFO") found = import_rpcs3_game(path.parent_path());
        else if (path.filename() == "openxmb-game.json" || path.filename().string().ends_with(".openxmb-game.json")) {
          found = game(read(path));
          if (found.executable.is_relative()) found.executable = path.parent_path() / found.executable;
          for (auto *media : {&found.icon, &found.background, &found.preview, &found.music, &found.working_directory})
            if (!media->empty() && media->is_relative()) *media = path.parent_path() / *media;
        } else if (path.extension() == ".desktop") found = desktop_file(path);
        else if (executable_file(*it)) { found.title = path.stem().string(); found.executable = path; found.working_directory = path.parent_path(); }
        else continue;
      } catch (const std::exception &) { continue; }
      if (found.title.empty() || found.executable.empty()) continue;
      const auto identity = found.executable.string() + '\n' + format_arguments(found.arguments);
      if (paths.insert(identity).second) { found.id.clear(); save_game(std::move(found)); ++count; }
      else if (path.filename() == "openxmb-game.json" || path.filename().string().ends_with(".openxmb-game.json")) {
        // A manifest carries richer metadata than the executable discovered in
        // the same directory. Retain play history and favorites when refreshing.
        for (const auto &saved : games()) if (saved.executable.string() + '\n' + format_arguments(saved.arguments) == identity) {
          found.id = saved.id; found.play_count = saved.play_count;
          found.last_played = saved.last_played; found.favorite = saved.favorite;
          save_game(std::move(found)); break;
        }
      }
    }
  }
  return count;
}

std::vector<CustomSection> Library::custom_sections() const {
  return items<CustomSection>(library(root_), "sections", [](const json &v) { return CustomSection{v.at("id"), v.at("title"), v.value("folder", ""), v.value("icon", "")}; });
}
CustomSection Library::save_section(CustomSection value) {
  valid_name(value.title);
  if (!fs::is_directory(value.folder)) throw std::runtime_error("Custom section folder must exist");
  if (value.id.empty()) value.id = identifier("section");
  auto state = library(root_); upsert(state, "sections", {{"id", value.id}, {"title", value.title}, {"folder", fs::weakly_canonical(value.folder).string()}, {"icon", value.icon.string()}}); save_library(root_, state); return value;
}
void Library::remove_section(const std::string &id) { erase(root_, "sections", id); }
std::vector<Friend> Library::friends() const {
  return items<Friend>(library(root_), "friends", [](const json &v) { return Friend{v.at("id"), v.at("name"), v.value("status", "Local friend"), v.value("present", false)}; });
}
Friend Library::save_friend(Friend value) {
  valid_name(value.name); if (value.id.empty()) value.id = identifier("friend");
  auto state = library(root_); upsert(state, "friends", {{"id", value.id}, {"name", value.name}, {"status", value.status}, {"present", value.present}}); save_library(root_, state); return value;
}
void Library::remove_friend(const std::string &id) { erase(root_, "friends", id); }
Account Library::account() const {
  const auto value = library(root_).value("account", json::object());
  return {value.value("display_name", active_profile().name), value.value("biography", ""), value.value("status", "Available locally"), value.value("avatar", "")};
}
void Library::save_account(Account value) {
  valid_name(value.display_name);
  auto state = library(root_); state["account"] = {{"display_name", value.display_name}, {"biography", value.biography}, {"status", value.status}, {"avatar", value.avatar.string()}}; save_library(root_, state);
}
std::vector<Trophy> Library::trophies() const {
  return items<Trophy>(library(root_), "trophies", [](const json &v) { return Trophy{v.at("id"), v.at("title"), v.value("game", ""), v.value("description", ""), v.value("grade", "bronze"), v.value("unlocked", false), v.value("unlocked_at", std::uint64_t{})}; });
}
Trophy Library::save_trophy(Trophy value) {
  valid_name(value.title); if (value.id.empty()) value.id = identifier("trophy");
  if (value.grade != "bronze" && value.grade != "silver" && value.grade != "gold" && value.grade != "platinum") throw std::runtime_error("Trophy grade must be bronze, silver, gold, or platinum");
  if (value.unlocked && !value.unlocked_at) value.unlocked_at = now();
  if (!value.unlocked) value.unlocked_at = 0;
  auto state = library(root_); upsert(state, "trophies", {{"id", value.id}, {"title", value.title}, {"game", value.game}, {"description", value.description}, {"grade", value.grade}, {"unlocked", value.unlocked}, {"unlocked_at", value.unlocked_at}}); save_library(root_, state); return value;
}
void Library::remove_trophy(const std::string &id) { erase(root_, "trophies", id); }
void Library::set_artwork_provider(bool enabled, std::string variable) {
  if (variable.empty() || !std::ranges::all_of(variable, [](unsigned char c) { return std::isalnum(c) || c == '_'; })) throw std::runtime_error("Invalid artwork API key variable name");
  auto state = library(root_); state["artwork_provider"] = {{"enabled", enabled}, {"key_variable", std::move(variable)}}; save_library(root_, state);
}
bool Library::artwork_provider_available() const {
  const auto provider = library(root_).value("artwork_provider", json::object());
  const auto variable = provider.value("key_variable", "RAWG_API_KEY");
  const auto *key = std::getenv(variable.c_str());
  return provider.value("enabled", false) && key && *key;
}

#ifdef OPENXMB_HAVE_CURL
namespace {
struct Download { std::vector<unsigned char> bytes; std::size_t limit; };
std::size_t receive(char *data, std::size_t size, std::size_t count, void *opaque) {
  auto &download = *static_cast<Download *>(opaque);
  if (size && count > download.limit / size) return 0;
  const auto bytes = size * count;
  if (bytes > download.limit - download.bytes.size()) return 0;
  download.bytes.insert(download.bytes.end(), data, data + bytes); return bytes;
}
std::vector<unsigned char> https(const std::string &url, std::size_t limit) {
  static const int initialized = [] { return curl_global_init(CURL_GLOBAL_DEFAULT); }();
  if (initialized != CURLE_OK) throw std::runtime_error("Artwork HTTPS initialization failed");
  auto *handle = curl_easy_init(); if (!handle) throw std::runtime_error("Artwork HTTPS initialization failed");
  Download download{{}, limit};
  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L); curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
  if(const auto* certificates=std::getenv("CURL_CA_BUNDLE");certificates&&*certificates)
    curl_easy_setopt(handle,CURLOPT_CAINFO,certificates);
  curl_easy_setopt(handle, CURLOPT_TIMEOUT, 15L); curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle, CURLOPT_USERAGENT, "OpenXMB/1.0"); curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, receive); curl_easy_setopt(handle, CURLOPT_WRITEDATA, &download);
  const auto status = curl_easy_perform(handle); long http{}; curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &http); curl_easy_cleanup(handle);
  if (status != CURLE_OK || http != 200) throw std::runtime_error("Artwork provider request failed; check configured key and network access");
  return download.bytes;
}
std::string encode(const std::string &value) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') result += c;
    else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
  }
  return result;
}
}
#endif
fs::path Library::fetch_artwork(const std::string &id) {
  valid_id(id);
  if (!artwork_provider_available()) throw std::runtime_error("Enable the optional RAWG provider and configure its key variable first");
#ifdef OPENXMB_HAVE_CURL
  const auto selected_profile = active_id(root_);
  const auto selected_directory = profile_directory(selected_profile);
  auto entries = games();
  const auto found = std::ranges::find_if(entries, [&](const Game &g) { return g.id == id; });
  if (found == entries.end()) throw std::runtime_error("Game does not exist");
  const auto provider = library(root_).at("artwork_provider");
  const auto *key = std::getenv(provider.at("key_variable").get<std::string>().c_str());
  const auto response = https("https://api.rawg.io/api/games?search=" + encode(found->title) + "&page_size=1&key=" + encode(key), 2 * 1024 * 1024);
  const auto metadata = json::parse(response);
  if (!metadata.contains("results") || metadata.at("results").empty()) throw std::runtime_error("Artwork provider found no matching game");
  const auto url = metadata.at("results").at(0).value("background_image", "");
  if (!url.starts_with("https://media.rawg.io/")) throw std::runtime_error("Artwork provider returned an unsupported image destination");
  const auto image = https(url, 16 * 1024 * 1024);
  const bool png = image.size() > 8 && image[0] == 137 && image[1] == 'P' && image[2] == 'N' && image[3] == 'G';
  const bool jpeg = image.size() > 3 && image[0] == 0xff && image[1] == 0xd8 && image[2] == 0xff;
  if (!png && !jpeg) throw std::runtime_error("Artwork response is not a PNG or JPEG image");
  if (active_id(root_) != selected_profile) throw std::runtime_error("Local profile changed during artwork lookup; retry in the selected profile");
  const auto folder = selected_directory / "artwork"; fs::create_directories(folder);
  const auto destination = folder / (id + '-' + identifier("image") + (png ? ".png" : ".jpg"));
  { std::ofstream output(destination, std::ios::binary); output.write(reinterpret_cast<const char *>(image.data()), static_cast<std::streamsize>(image.size())); if (!output) throw std::runtime_error("Cannot save downloaded artwork"); }
  found->background = destination; if (found->icon.empty()) found->icon = destination; save_game(*found); return destination;
#else
  (void)id;
  throw std::runtime_error("This build has no optional HTTPS artwork support; import local artwork instead");
#endif
}
} // namespace openxmb::desktop
