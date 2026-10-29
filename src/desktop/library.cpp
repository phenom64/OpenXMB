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
#ifndef OPENXMB_VERSION_STRING
#define OPENXMB_VERSION_STRING "development"
#endif
#include "openxmb/desktop/steam.hpp"

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
#include <string_view>
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
std::string path_text(const fs::path &path) {
  const auto value = path.u8string();
  return {value.begin(), value.end()};
}
fs::path text_path(const std::string &value) { return fs::path(std::u8string(value.begin(), value.end())); }
std::optional<fs::path> environment_path(const char *name) {
#ifdef _WIN32
  const std::wstring key(name, name + std::strlen(name));
  if (const auto *value = _wgetenv(key.c_str()); value && *value) return fs::path(value);
#else
  if (const auto *value = std::getenv(name); value && *value) return text_path(value);
#endif
  return {};
}
std::string path_identity(const fs::path &path) {
  std::error_code error;
  auto normalized = fs::weakly_canonical(path, error);
  if (error) normalized = fs::absolute(path).lexically_normal();
#ifdef _WIN32
  // Windows path identity is case-insensitive; keep non-ASCII code points
  // intact instead of applying a byte-wise case conversion to UTF-8.
  auto native = normalized.native();
  std::wstring folded(native.size(), L'\0');
  if (!native.empty() && LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, native.data(), static_cast<int>(native.size()),
      folded.data(), static_cast<int>(folded.size()), nullptr, nullptr, 0)) native = std::move(folded);
  return path_text(fs::path(native));
#else
  return path_text(normalized);
#endif
}
std::atomic<std::uint64_t> serial{};
std::string game_identity(const Game& game) {
  if(!game.game_directory.empty()) return "rpcs3\n"+path_identity(game.game_directory);
  if(!game.runner.empty() && game.runner!="native" && !game.content.empty())
    return game.runner+'\n'+(game.runner=="steam"?path_text(game.content):path_identity(game.content))+'\n'+(game.core.empty()?std::string{}:path_identity(game.core));
  return path_text(game.executable)+'\n'+format_arguments(game.arguments);
}
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
  json automatic=json::object();
  for(const auto& [role,path]:g.automatic_artwork)automatic[role]=path_text(path);
  return {{"id", g.id}, {"title", g.title}, {"description", g.description}, {"genre", g.genre}, {"developer", g.developer}, {"category", g.category},
    {"executable", path_text(g.executable)}, {"arguments", g.arguments}, {"working_directory", path_text(g.working_directory)}, {"icon", path_text(g.icon)},
    {"background", path_text(g.background)}, {"preview", path_text(g.preview)}, {"music", path_text(g.music)}, {"cover",path_text(g.cover)}, {"logo",path_text(g.logo)}, {"year", g.year}, {"favorite", g.favorite},
    {"play_count", g.play_count}, {"last_played", g.last_played}, {"platform", g.platform}, {"title_id", g.title_id}, {"game_directory", path_text(g.game_directory)}, {"runner",g.runner}, {"content",path_text(g.content)}, {"core",path_text(g.core)}, {"automatic_artwork",std::move(automatic)},
    {"launch_override",{{"mode",g.launch_override.mode==GameLaunchMode::direct?"direct":"provider"},
      {"executable",path_text(g.launch_override.executable)},{"arguments",g.launch_override.arguments},
      {"working_directory",path_text(g.launch_override.working_directory)}}}};
}
Game game(const json &value) {
  Game g;
  g.id = value.value("id", ""); g.title = value.value("title", ""); g.description = value.value("description", "");
  g.genre = value.value("genre", ""); g.developer = value.value("developer", ""); g.category = value.value("category", "");
  g.executable = text_path(value.value("executable", "")); g.arguments = value.value("arguments", std::vector<std::string>{});
  g.working_directory = text_path(value.value("working_directory", "")); g.icon = text_path(value.value("icon", "")); g.background = text_path(value.value("background", ""));
  g.preview = text_path(value.value("preview", "")); g.music = text_path(value.value("music", "")); g.year = value.value("year", 0); g.favorite = value.value("favorite", false);
  g.play_count = value.value("play_count", std::uint64_t{}); g.last_played = value.value("last_played", std::uint64_t{});
  g.runner=value.value("runner","");g.content=text_path(value.value("content",""));g.core=text_path(value.value("core",""));
  if(value.contains("launch_override")) {
    const auto& launch=value.at("launch_override");
    if(!launch.is_object())throw std::runtime_error("Invalid game launch settings");
    const auto mode=launch.value("mode","provider");
    if(mode!="provider"&&mode!="direct")throw std::runtime_error("Unknown game launch mode");
    g.launch_override.mode=mode=="direct"?GameLaunchMode::direct:GameLaunchMode::provider;
    g.launch_override.executable=text_path(launch.value("executable",""));
    g.launch_override.working_directory=text_path(launch.value("working_directory",""));
    g.launch_override.arguments=launch.value("arguments",std::vector<std::string>{});
  }
  g.cover=text_path(value.value("cover",""));g.logo=text_path(value.value("logo",""));
  if(value.contains("automatic_artwork")) {
    const auto& automatic=value.at("automatic_artwork");
    if(!automatic.is_object())throw std::runtime_error("Invalid automatic artwork metadata");
    for(const auto* role:{"icon","background","preview","music","cover","logo"})if(automatic.contains(role))g.automatic_artwork[role]=text_path(automatic.at(role).get<std::string>());
  }
  g.platform = value.value("platform", ""); g.title_id = value.value("title_id", ""); g.game_directory = text_path(value.value("game_directory", ""));
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
std::vector<fs::path> rpcs3_data_directories(const fs::path& selected_executable = {}) {
  std::vector<fs::path> candidates;
  // RPCS3_DATA_DIR is OpenXMB's explicit data-root override. The remaining
  // locations follow RPCS3's fs::get_config_dir(), plus its Flatpak sandbox.
  if (const auto value = environment_path("RPCS3_DATA_DIR")) candidates.emplace_back(*value);
  const auto executable = rpcs3_executable(selected_executable);
  if (executable) candidates.emplace_back(executable->parent_path() / "portable");
#ifdef _WIN32
  if (const auto value = environment_path("RPCS3_CONFIG_DIR")) candidates.emplace_back(*value);
  if (executable) candidates.emplace_back(executable->parent_path());
  // Retain detection of older user installations without migrating any files.
  if (const auto value = environment_path("APPDATA")) candidates.emplace_back(*value / "rpcs3");
#elif defined(__APPLE__)
  if (const auto value = environment_path("HOME")) candidates.emplace_back(*value / "Library/Application Support/rpcs3");
#else
  if (const auto value = environment_path("XDG_CONFIG_HOME")) candidates.emplace_back(*value / "rpcs3");
  else if (const auto value = environment_path("HOME")) candidates.emplace_back(*value / ".config/rpcs3");
  if (const auto value = environment_path("HOME")) candidates.emplace_back(*value / ".var/app/net.rpcs3.RPCS3/config/rpcs3");
#endif
  std::set<std::string> seen;
  std::vector<fs::path> result;
  for (const auto &candidate : candidates) {
    std::error_code error;
    if (fs::is_directory(candidate, error) && seen.insert(path_identity(candidate)).second)
      result.push_back(fs::weakly_canonical(candidate));
  }
  return result;
}
std::string bounded_text(const fs::path &file, std::size_t limit) {
  if (!fs::is_regular_file(file) || fs::file_size(file) > limit) throw std::runtime_error("RPCS3 metadata is missing or too large");
  std::ifstream input(file, std::ios::binary);
  std::string result(limit + 1, '\0');
  input.read(result.data(), static_cast<std::streamsize>(result.size()));
  const auto size = static_cast<std::size_t>(input.gcount());
  if (input.bad() || size > limit) throw std::runtime_error("Cannot read bounded RPCS3 metadata");
  result.resize(size);
  return result;
}
void append_utf8(std::string &text, std::uint32_t codepoint) {
  if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) throw std::runtime_error("Invalid YAML Unicode escape");
  if (codepoint < 0x80) text += static_cast<char>(codepoint);
  else if (codepoint < 0x800) { text += static_cast<char>(0xc0 | (codepoint >> 6)); text += static_cast<char>(0x80 | (codepoint & 63)); }
  else if (codepoint < 0x10000) { text += static_cast<char>(0xe0 | (codepoint >> 12)); text += static_cast<char>(0x80 | ((codepoint >> 6) & 63)); text += static_cast<char>(0x80 | (codepoint & 63)); }
  else { text += static_cast<char>(0xf0 | (codepoint >> 18)); text += static_cast<char>(0x80 | ((codepoint >> 12) & 63)); text += static_cast<char>(0x80 | ((codepoint >> 6) & 63)); text += static_cast<char>(0x80 | (codepoint & 63)); }
}
void valid_yaml_text(std::string_view text) {
  for (std::size_t at = 0; at < text.size();) {
    const auto lead = static_cast<unsigned char>(text[at++]);
    if (lead < 0x20 || lead == 0x7f) throw std::runtime_error("Control characters are unsupported in RPCS3 paths");
    if (lead < 0x80) continue;
    const unsigned length = lead >= 0xc2 && lead <= 0xdf ? 2 : lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
    if (!length || text.size() - at < length - 1) throw std::runtime_error("Invalid YAML UTF-8");
    std::uint32_t codepoint = lead & (0x7f >> length);
    for (unsigned i = 1; i < length; ++i) {
      const auto next = static_cast<unsigned char>(text[at++]);
      if ((next & 0xc0) != 0x80) throw std::runtime_error("Invalid YAML UTF-8");
      codepoint = (codepoint << 6) | (next & 63);
    }
    if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) || (length == 4 && codepoint < 0x10000) ||
        codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) throw std::runtime_error("Invalid YAML UTF-8 codepoint");
  }
}
bool yaml_space(char c) { return c == ' ' || c == '\t'; }
void yaml_spaces(std::string_view line, std::size_t &at) { while (at < line.size() && yaml_space(line[at])) ++at; }
std::string yaml_scalar(std::string_view line, std::size_t &at, bool key) {
  if (at == line.size()) throw std::runtime_error("Missing RPCS3 registry scalar");
  std::string result;
  const auto quote = line[at] == '\'' || line[at] == '"' ? line[at++] : '\0';
  if (quote) {
    bool closed = false;
    while (at < line.size()) {
      char c = line[at++];
      if (c == quote) {
        if (quote == '\'' && at < line.size() && line[at] == '\'') { result += '\''; ++at; continue; }
        closed = true; break;
      }
      if (quote != '"' || c != '\\') { result += c; continue; }
      if (at == line.size()) throw std::runtime_error("Truncated YAML escape");
      c = line[at++];
      switch (c) {
      case '"': case '\\': case '/': result += c; break;
      case ' ': result += ' '; break;
      case '0': result += '\0'; break; case 'a': result += '\a'; break;
      case 'b': result += '\b'; break; case 't': result += '\t'; break;
      case 'n': result += '\n'; break; case 'v': result += '\v'; break;
      case 'f': result += '\f'; break; case 'r': result += '\r'; break;
      case 'e': result += '\x1b'; break;
      case 'N': append_utf8(result, 0x85); break; case '_': append_utf8(result, 0xa0); break;
      case 'L': append_utf8(result, 0x2028); break; case 'P': append_utf8(result, 0x2029); break;
      case 'x': case 'u': case 'U': {
        const unsigned digits = c == 'x' ? 2 : c == 'u' ? 4 : 8;
        if (line.size() - at < digits) throw std::runtime_error("Truncated YAML Unicode escape");
        std::uint32_t codepoint = 0;
        for (unsigned i = 0; i < digits; ++i) {
          const auto hex = line[at++];
          const int value = hex >= '0' && hex <= '9' ? hex - '0' : hex >= 'a' && hex <= 'f' ? hex - 'a' + 10 : hex >= 'A' && hex <= 'F' ? hex - 'A' + 10 : -1;
          if (value < 0) throw std::runtime_error("Invalid YAML Unicode escape");
          codepoint = (codepoint << 4) | static_cast<unsigned>(value);
        }
        append_utf8(result, codepoint); break;
      }
      default: throw std::runtime_error("Unsupported YAML escape");
      }
    }
    if (!closed) throw std::runtime_error("Unterminated YAML scalar");
  } else {
    const auto first = line[at];
    if (std::string_view("!&*{}[],#|>\"'%@`").find(first) != std::string_view::npos ||
        ((first == '-' || first == '?' || first == ':') && (at + 1 == line.size() || yaml_space(line[at + 1]))))
      throw std::runtime_error("Unsupported RPCS3 registry YAML construct");
    const auto start = at;
    while (at < line.size()) {
      if (line[at] == ':' && (at + 1 == line.size() || yaml_space(line[at + 1]))) {
        if (key) break;
        throw std::runtime_error("Nested YAML mappings are unsupported");
      }
      if (line[at] == '#' && at > start && yaml_space(line[at - 1])) break;
      ++at;
    }
    auto end = at;
    while (end > start && yaml_space(line[end - 1])) --end;
    result = line.substr(start, end - start);
  }
  if (result.empty() || result.size() > 32768) throw std::runtime_error("Invalid RPCS3 registry scalar length");
  valid_yaml_text(result);
  return result;
}
std::vector<fs::path> read_rpcs3_registry(const fs::path &file) {
  // RPCS3's yaml-cpp emitter writes a block map<string,string>. Deliberately
  // reject collections, aliases, tags, complex keys and multiline scalars;
  // never reinterpret an arbitrary YAML document as a list of game paths.
  const auto bytes = bounded_text(file, 4 * 1024 * 1024);
  std::string_view input(bytes);
  if (input.starts_with("\xef\xbb\xbf")) input.remove_prefix(3);
  std::vector<fs::path> paths;
  std::set<std::string> keys;
  while (!input.empty()) {
    const auto newline = input.find('\n');
    auto line = input.substr(0, newline);
    input = newline == std::string_view::npos ? std::string_view{} : input.substr(newline + 1);
    if (line.ends_with('\r')) line.remove_suffix(1);
    if (line.size() > 131072) throw std::runtime_error("RPCS3 registry line is too long");
    std::size_t at = 0; yaml_spaces(line, at);
    if (at == line.size() || line[at] == '#') continue;
    if (at != 0) throw std::runtime_error("Indented RPCS3 registry mappings are unsupported");
    if (line == "{}" && keys.empty() && input.empty()) return {};
    const auto key = yaml_scalar(line, at, true);
    yaml_spaces(line, at);
    if (at == line.size() || line[at++] != ':' || (at < line.size() && !yaml_space(line[at]))) throw std::runtime_error("Invalid RPCS3 registry mapping");
    yaml_spaces(line, at);
    const auto value = yaml_scalar(line, at, false);
    yaml_spaces(line, at);
    if (at < line.size() && line[at] != '#') throw std::runtime_error("Unexpected RPCS3 registry trailing content");
    if (keys.size() >= 10000 || !keys.insert(key).second) throw std::runtime_error("Too many or duplicate RPCS3 registry keys");
    const auto path = text_path(value);
    if (path.is_absolute()) paths.push_back(path);
  }
  return paths;
}
std::vector<fs::path> rpcs3_registered_paths(const fs::path& selected_executable = {}) {
  std::vector<fs::path> result;
  for (const auto &directory : rpcs3_data_directories(selected_executable)) {
    std::vector<fs::path> registries;
#ifdef _WIN32
    registries.push_back(directory / "config/games.yml");
#endif
    registries.push_back(directory / "games.yml");
    for (const auto &registry : registries) {
      try {
        const auto paths = read_rpcs3_registry(registry);
        result.insert(result.end(), paths.begin(), paths.end());
      } catch (const std::exception &) { /* An unreadable registry cannot hide other installations. */ }
    }
  }
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
std::optional<fs::path> rpcs3_executable(const fs::path& selected_executable) {
  const auto usable = [](const fs::path &path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error)) return false;
#ifdef _WIN32
    return true;
#else
    return ::access(path.c_str(), X_OK) == 0;
#endif
  };
  if(!selected_executable.empty()) {
    if(selected_executable.native().size()>32768||selected_executable.native().find(fs::path::value_type{})!=fs::path::string_type::npos||!usable(selected_executable))
      throw std::runtime_error("The selected RPCS3 executable is unavailable; choose its installed executable in Game Settings");
    return fs::absolute(selected_executable);
  }
  // Keep metadata/portable-root discovery on the same bounded executable
  // candidates and override precedence as launch planning. Runner discovery
  // reads Game DTOs only and never calls back into this helper.
  for(const auto& runner:discover_runners())if(runner.id=="rpcs3")
    return runner?std::optional<fs::path>{runner.executable}:std::nullopt;
  return {};
}
std::vector<fs::path> rpcs3_game_roots(const fs::path& selected_executable) {
  std::vector<fs::path> candidates;
  for (const auto &directory : rpcs3_data_directories(selected_executable)) {
    candidates.emplace_back(directory / "dev_hdd0/game");
    candidates.emplace_back(directory / "games");
  }
  std::vector<fs::path> result;
  for (const auto &candidate : candidates) {
    std::error_code error;
    if (!fs::is_directory(candidate, error)) continue;
    const auto normalized = fs::weakly_canonical(candidate, error);
    if (!error && std::ranges::find(result, normalized) == result.end()) result.push_back(normalized);
  }
  return result;
}
Game import_rpcs3_game(const fs::path &directory,const fs::path& selected_executable) {
  auto folder = fs::absolute(directory).lexically_normal();
  if (folder.filename() == "PARAM.SFO") folder = folder.parent_path();
  else if (folder.filename() == "EBOOT.BIN" && folder.parent_path().filename() == "USRDIR") folder = folder.parent_path().parent_path();
  if (!fs::is_regular_file(folder / "PARAM.SFO") && fs::is_regular_file(folder / "PS3_GAME/PARAM.SFO")) folder /= "PS3_GAME";
  folder = fs::weakly_canonical(folder);
  const auto metadata = read_param_sfo(folder / "PARAM.SFO");
  const auto value = [&](const char *key) { const auto found = metadata.find(key); return found == metadata.end() ? std::string{} : found->second; };
  const auto boot = folder / "USRDIR/EBOOT.BIN";
  if ((value("CATEGORY") != "DG" && value("CATEGORY") != "HG") || !fs::is_regular_file(boot))
    throw std::runtime_error("RPCS3 import requires a bootable DG disc or HG HDD title with USRDIR/EBOOT.BIN");
  Game game;
  game.platform = "PlayStation 3 (RPCS3)"; game.runner="rpcs3";game.content=boot;
  game.title = value("TITLE").empty() ? path_text(folder.filename()) : value("TITLE");
  game.title_id = value("TITLE_ID");
  game.game_directory = folder;
  game.description = game.platform + "  " + game.title_id + (value("APP_VER").empty() ? "" : "  v" + value("APP_VER"));
  game.category = "PlayStation 3";
  game.executable = rpcs3_executable(selected_executable).value_or(fs::path("rpcs3"));
  game.arguments = {"--no-gui", path_text(boot)};
  game.working_directory = folder;
  for (const auto &[name, target] : std::initializer_list<std::pair<const char *, fs::path *>>{{"ICON0.PNG", &game.icon}, {"PIC1.PNG", &game.background}, {"ICON1.PAM", &game.preview}, {"SND0.AT3", &game.music}})
    if (fs::is_regular_file(folder / name)) *target = folder / name;
  if (game.background.empty() && fs::is_regular_file(folder / "PIC0.PNG")) game.background = folder / "PIC0.PNG";
  return game;
}
Game launch_game_target(const Game &game) {
  if(game.launch_override.mode==GameLaunchMode::provider)return game;
  if(game.launch_override.mode!=GameLaunchMode::direct||game.runner!="steam")
    throw std::runtime_error("Direct executable launch settings are supported for Steam games");
  const auto& launch=game.launch_override;
  for(const auto* path:{&launch.executable,&launch.working_directory})
    if(path->native().find(fs::path::value_type{})!=fs::path::string_type::npos||path->native().size()>32768)
      throw std::runtime_error("A direct launch path contains NUL or exceeds 32768 characters");
  std::error_code error;
  if(!launch.executable.is_absolute()||!fs::is_regular_file(launch.executable,error)||error)
    throw std::runtime_error("Choose an existing game executable with a full file path");
#ifdef _WIN32
  if(lower(launch.executable.extension().string())!=".exe")throw std::runtime_error("Choose a Windows game executable (.exe)");
#else
  if(::access(launch.executable.c_str(),X_OK)!=0)throw std::runtime_error("The chosen game file is not executable");
#endif
  if(!launch.working_directory.empty()&&(!launch.working_directory.is_absolute()||!fs::is_directory(launch.working_directory,error)||error))
    throw std::runtime_error("Choose an existing working folder with a full path, or leave it empty");
  if(launch.arguments.size()>255)throw std::runtime_error("Use at most 255 direct launch arguments");
  std::size_t bytes=0;
  for(const auto& argument:launch.arguments) {
    if(argument.find('\0')!=std::string::npos||argument.size()>32768)throw std::runtime_error("A direct launch argument contains NUL or exceeds 32768 bytes");
    bytes+=argument.size();
  }
  if(bytes>256*1024)throw std::runtime_error("Direct launch arguments exceed 256 KiB");
  Game target=game;target.runner="native";target.executable=launch.executable;target.arguments=launch.arguments;
  target.working_directory=launch.working_directory.empty()?launch.executable.parent_path():launch.working_directory;
  target.content.clear();target.core.clear();target.game_directory.clear();target.launch_override={};
  return target;
}
void launch_game(const Game &entry) {
  const auto game=launch_game_target(entry);
  if (game.executable.empty()) throw std::runtime_error("Set this game's executable before launching");
  std::vector<std::string> arguments{path_text(game.executable)};
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
std::vector<Game> Library::games() const {
  return games(active_id(root_));
}
std::vector<Game> Library::games(const std::string &profile_id) const {
  valid_id(profile_id);
  const auto entries=profiles();
  if(std::ranges::none_of(entries,[&](const Profile& value){return value.id==profile_id;}))
    throw std::runtime_error("Profile does not exist");
  auto result=items<Game>(read(profile_directory(profile_id)/"library.json",{{"version",1}}),"games",game);
  std::erase_if(result,[](const Game& value){return !steam_game_is_playable(value);});
  return result;
}
Game Library::save_game(Game value) {
  valid_name(value.title);
  if (value.id.empty()) value.id = identifier("game");
  valid_id(value.id);
  if (value.arguments.size() > 256) throw std::runtime_error("Too many launch arguments");
  if(value.launch_override.mode!=GameLaunchMode::provider&&value.launch_override.mode!=GameLaunchMode::direct)throw std::runtime_error("Unknown game launch mode");
  if(value.launch_override.mode==GameLaunchMode::direct&&value.runner!="steam")throw std::runtime_error("Direct launch overrides require a Steam library entry");
  if(value.launch_override.arguments.size()>255)throw std::runtime_error("Too many direct launch arguments");
  auto state = library(root_); upsert(state, "games", game_json(value)); save_library(root_, state); return value;
}
void Library::remove_game(const std::string &id) { erase(root_, "games", id); }
void Library::mark_played(const std::string &id) {
  for (auto value : games()) if (value.id == id) { ++value.play_count; value.last_played = now(); save_game(std::move(value)); return; }
  throw std::runtime_error("Game does not exist");
}
std::vector<fs::path> Library::game_roots() const {
  std::vector<fs::path> result;
  for (const auto &path : library(root_).value("game_roots", std::vector<std::string>{})) result.emplace_back(text_path(path));
  return result;
}
void Library::add_game_root(const fs::path &path) {
  if (!fs::is_directory(path)) throw std::runtime_error("Game root must be an existing directory");
  auto state = library(root_); auto roots = state.value("game_roots", std::vector<std::string>{});
  const auto value = path_text(fs::weakly_canonical(path));
  if (std::ranges::find(roots, value) == roots.end()) roots.push_back(value);
  state["game_roots"] = roots; save_library(root_, state);
}
void Library::remove_game_root(const fs::path &path) {
  auto state = library(root_); auto roots = state.value("game_roots", std::vector<std::string>{});
  const auto value = path_text(fs::weakly_canonical(path)); std::erase(roots, value); state["game_roots"] = roots; save_library(root_, state);
}
GameDiscovery discover_games(const std::vector<fs::path>& configured_roots, GameDiscoveryOptions options) {
  // Validate an explicit selection before scanning. It never falls back to a
  // different installation when the chosen emulator has moved or disappeared.
  if(!options.rpcs3_executable.empty())options.rpcs3_executable=*rpcs3_executable(options.rpcs3_executable);
  GameDiscovery result;
  std::size_t examined=0;
  std::set<std::string> seen;
  const auto accept=[&](Game value,bool manifest=false) {
    const auto identity=game_identity(value);
    if(seen.insert(identity).second)result.push_back({std::move(value),manifest});
  };
  if(options.registered_rpcs3) for (const auto &path : rpcs3_registered_paths(options.rpcs3_executable)) {
    if (++examined > 10000) throw std::runtime_error("Game scan exceeded 10000 entries; choose narrower roots");
    Game found;
    try { found = import_rpcs3_game(path,options.rpcs3_executable); } catch (const std::exception &) { continue; }
    accept(std::move(found));
  }
  auto roots = configured_roots;
  if(options.registered_rpcs3) {
    const auto detected = rpcs3_game_roots(options.rpcs3_executable); roots.insert(roots.end(), detected.begin(), detected.end());
  }
  if(options.installed_steam)for(auto& candidate:discover_steam_games())accept(std::move(candidate.game),candidate.refresh_metadata);
  std::set<std::string> visited;
  for (const auto &root : roots) {
    if (!visited.insert(path_identity(root)).second) continue;
    if (fs::is_regular_file(root / "PARAM.SFO") || fs::is_regular_file(root / "PS3_GAME/PARAM.SFO")) {
      Game found;
      try { found = import_rpcs3_game(root,options.rpcs3_executable); } catch (const std::exception &) { continue; }
      accept(std::move(found));
      continue;
    }
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
      if (++examined > 10000) throw std::runtime_error("Game scan exceeded 10000 entries; choose narrower roots");
      if (it.depth() > 5) { it.disable_recursion_pending(); continue; }
      if (it->is_symlink()) { it.disable_recursion_pending(); continue; }
      Game found;
      const auto path = it->path();
      try {
        if (it->is_directory() && (fs::is_regular_file(path / "PARAM.SFO") || fs::is_regular_file(path / "PS3_GAME/PARAM.SFO"))) {
          it.disable_recursion_pending();
          found = import_rpcs3_game(path,options.rpcs3_executable);
        } else if (path.filename() == "PARAM.SFO") found = import_rpcs3_game(path.parent_path(),options.rpcs3_executable);
        else if (path.filename() == "openxmb-game.json" || path.filename().string().ends_with(".openxmb-game.json")) {
          found = game(read(path));
          if (!found.executable.empty() && found.executable.is_relative()) found.executable = path.parent_path() / found.executable;
          for (auto *media : {&found.icon, &found.background, &found.preview, &found.music, &found.cover, &found.logo, &found.working_directory, &found.core})
            if (!media->empty() && media->is_relative()) *media = path.parent_path() / *media;
          if(found.runner!="steam" && !found.content.empty() && found.content.is_relative()) found.content=path.parent_path()/found.content;
        } else if (path.extension() == ".desktop") found = desktop_file(path);
        else if (executable_file(*it)) { found.title = path.stem().string(); found.executable = path; found.working_directory = path.parent_path(); }
        else continue;
      } catch (const std::exception &) { continue; }
      accept(std::move(found), path.filename() == "openxmb-game.json" || path.filename().string().ends_with(".openxmb-game.json"));
    }
  }
  return result;
}

std::size_t Library::import_games(const GameDiscovery& discovered) {
  // Persistence merges use every stored entry, including provider support
  // products hidden from the games view. Never serialize a filtered view.
  auto existing = items<Game>(library(root_),"games",game);
  const auto native = [](const Game &game) { return game.platform == "PlayStation 3 (RPCS3)" && !game.game_directory.empty(); };
  const auto identity = [&](const Game &game) {
    // The installed emulator executable and launch options can change. A
    // canonical native metadata directory remains the same library title.
    return game_identity(game);
  };
  std::map<std::string, std::size_t> paths;
  for (std::size_t i = 0; i < existing.size(); ++i) paths.emplace(identity(existing[i]), i);
  std::size_t count = 0;
  const auto accept = [&](Game found, bool manifest = false) {
    if(!steam_game_is_playable(found))return;
    if (found.title.empty() || (found.executable.empty() && (found.runner.empty() || found.content.empty()))) return;
    const std::array<std::pair<const char*,fs::path Game::*>,6> roles{{{"icon",&Game::icon},{"background",&Game::background},{"preview",&Game::preview},{"music",&Game::music},{"cover",&Game::cover},{"logo",&Game::logo}}};
    for(const auto& [role,member]:roles)found.automatic_artwork[role]=found.*member;
    const auto key = identity(found);
    if (const auto match = paths.find(key); match != paths.end()) {
      if (!native(found) && !manifest) return;
      const auto &saved = existing[match->second];
      found.id = saved.id; found.play_count = saved.play_count;
      found.last_played = saved.last_played; found.favorite = saved.favorite;
      found.launch_override = saved.launch_override;
      if (native(found)) {
        const auto preserve = [&](fs::path Game::*member, const char* role, std::initializer_list<const char *> filenames) {
          const auto &selected = saved.*member;
          if (selected.empty()) return;
          const auto chosen = path_identity(selected);
          if(const auto automatic=saved.automatic_artwork.find(role);automatic!=saved.automatic_artwork.end()) {
            if(automatic->second.empty() || chosen!=path_identity(automatic->second))found.*member=selected;
            return;
          }
          // Native role paths identify prior automatic choices even when the
          // asset was removed. Any other saved path is a user-selected asset.
          if (std::ranges::none_of(filenames, [&](const auto *name) { return chosen == path_identity(saved.game_directory / name); })) found.*member = selected;
        };
        preserve(&Game::icon, "icon", {"ICON0.PNG"}); preserve(&Game::background, "background", {"PIC1.PNG", "PIC0.PNG"});
        preserve(&Game::preview, "preview", {"ICON1.PAM"}); preserve(&Game::music, "music", {"SND0.AT3"});
      } else for(const auto& [role,member]:roles) {
        const auto& selected=saved.*member;
        const auto automatic=saved.automatic_artwork.find(role);
        if(!selected.empty() && (automatic==saved.automatic_artwork.end() || automatic->second.empty() || path_identity(selected)!=path_identity(automatic->second)))found.*member=selected;
      }
      for(const auto& [role,member]:roles)if((found.*member).empty() && !(saved.*member).empty()) {
        // Missing native PS3 media is a source update, not a stale provider
        // cache. Manual choices for these roles were retained above.
        if(native(found)&&member!=&Game::cover&&member!=&Game::logo)continue;
        found.*member=saved.*member;
        if(const auto previous=saved.automatic_artwork.find(role);previous!=saved.automatic_artwork.end())found.automatic_artwork[role]=previous->second;
      }
      existing[match->second] = save_game(std::move(found));
      return;
    }
    found.id.clear();
    auto saved = save_game(std::move(found));
    paths.emplace(key, existing.size()); existing.push_back(std::move(saved)); ++count;
  };
  for (const auto& candidate:discovered) accept(candidate.game,candidate.refresh_metadata);
  return count;
}

std::size_t Library::scan_games(GameDiscoveryOptions options) {
  return import_games(discover_games(game_roots(),options));
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
  curl_easy_setopt(handle, CURLOPT_USERAGENT, "OpenXMB/" OPENXMB_VERSION_STRING); curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, receive); curl_easy_setopt(handle, CURLOPT_WRITEDATA, &download);
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
