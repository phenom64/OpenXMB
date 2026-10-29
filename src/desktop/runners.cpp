#include "openxmb/desktop/runners.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace openxmb::desktop {
namespace {
namespace fs = std::filesystem;
struct Definition {
  const char* id;
  const char* name;
  const char* environment;
  std::vector<fs::path> names;
};
std::string text(const fs::path& path) { const auto value = path.u8string(); return {value.begin(), value.end()}; }
std::string lower(std::string value) { std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }); return value; }
void valid_path(const fs::path& path) {
  if (path.native().find(fs::path::value_type{}) != fs::path::string_type::npos || path.native().size() > 32768) throw std::runtime_error("A runner path contains NUL or exceeds 32768 characters");
}
void valid_arguments(const std::vector<std::string>& arguments) {
  if (arguments.size() > 256) throw std::runtime_error("Launch plans support at most 256 arguments including the executable");
  std::size_t bytes = 0;
  for (const auto& argument : arguments) {
    if (argument.find('\0') != std::string::npos || argument.size() > 32768) throw std::runtime_error("A launch argument contains NUL or exceeds 32768 bytes");
    bytes += argument.size(); (void)nlohmann::json(argument).dump();
  }
  if (bytes > 256 * 1024) throw std::runtime_error("Launch plan arguments exceed 256 KiB");
}
bool usable(const fs::path& path) {
  valid_path(path); std::error_code error;
  if (!fs::is_regular_file(path, error)) return false;
#ifdef _WIN32
  return true;
#else
  return access(path.c_str(), X_OK) == 0;
#endif
}
std::optional<fs::path> environment_path(const char* name) {
#ifdef _WIN32
  const std::string key(name); const std::wstring wide(key.begin(), key.end());
  if (const auto* value = _wgetenv(wide.c_str()); value && *value) return fs::path(value);
#else
  if (const auto* value = std::getenv(name); value && *value) return fs::path(value);
#endif
  return {};
}
std::vector<Definition> definitions() {
#ifdef _WIN32
  return {
    {"rpcs3", "RPCS3", "RPCS3_EXECUTABLE", {"rpcs3.exe", "RPCS3.exe", "RPCS3/rpcs3.exe"}},
    {"pcsx2", "PCSX2", "PCSX2_EXECUTABLE", {"pcsx2-qt.exe", "pcsx2-qt64.exe", "pcsx2.exe", "PCSX2/pcsx2-qt.exe"}},
    {"steam", "Steam", "STEAM_EXECUTABLE", {"steam.exe", "Steam/steam.exe"}},
    {"retroarch", "RetroArch", "RETROARCH_EXECUTABLE", {"retroarch.exe", "RetroArch/retroarch.exe"}},
    {"dolphin", "Dolphin", "DOLPHIN_EXECUTABLE", {"Dolphin.exe", "Dolphin-x64/Dolphin.exe", "Dolphin Emulator/Dolphin.exe"}},
    {"duckstation", "DuckStation", "DUCKSTATION_EXECUTABLE", {"duckstation-qt-x64-ReleaseLTCG.exe", "duckstation-qt.exe", "duckstation.exe", "DuckStation/duckstation-qt-x64-ReleaseLTCG.exe"}},
    {"ppsspp", "PPSSPP", "PPSSPP_EXECUTABLE", {"PPSSPPWindows64.exe", "PPSSPPWindows.exe", "PPSSPP/PPSSPPWindows64.exe"}}
  };
#elif defined(__APPLE__)
  return {
    {"rpcs3", "RPCS3", "RPCS3_EXECUTABLE", {"rpcs3", "RPCS3.app/Contents/MacOS/rpcs3"}},
    {"pcsx2", "PCSX2", "PCSX2_EXECUTABLE", {"pcsx2-qt", "PCSX2.app/Contents/MacOS/PCSX2", "PCSX2.app/Contents/MacOS/pcsx2-qt"}},
    {"steam", "Steam", "STEAM_EXECUTABLE", {"steam", "Steam.app/Contents/MacOS/steam_osx"}},
    {"retroarch", "RetroArch", "RETROARCH_EXECUTABLE", {"retroarch", "RetroArch.app/Contents/MacOS/RetroArch"}},
    {"dolphin", "Dolphin", "DOLPHIN_EXECUTABLE", {"dolphin-emu", "Dolphin.app/Contents/MacOS/Dolphin"}},
    {"duckstation", "DuckStation", "DUCKSTATION_EXECUTABLE", {"duckstation-qt", "DuckStation.app/Contents/MacOS/DuckStation"}},
    {"ppsspp", "PPSSPP", "PPSSPP_EXECUTABLE", {"PPSSPPSDL", "ppsspp", "PPSSPP.app/Contents/MacOS/PPSSPPSDL", "PPSSPP.app/Contents/MacOS/PPSSPP"}}
  };
#else
  return {
    {"rpcs3", "RPCS3", "RPCS3_EXECUTABLE", {"rpcs3", "rpcs3.AppImage"}},
    {"pcsx2", "PCSX2", "PCSX2_EXECUTABLE", {"pcsx2-qt", "pcsx2", "PCSX2.AppImage"}},
    {"steam", "Steam", "STEAM_EXECUTABLE", {"steam", "steam-runtime"}},
    {"retroarch", "RetroArch", "RETROARCH_EXECUTABLE", {"retroarch"}},
    {"dolphin", "Dolphin", "DOLPHIN_EXECUTABLE", {"dolphin-emu", "dolphin-emu-qt2"}},
    {"duckstation", "DuckStation", "DUCKSTATION_EXECUTABLE", {"duckstation-qt", "duckstation", "DuckStation.AppImage"}},
    {"ppsspp", "PPSSPP", "PPSSPP_EXECUTABLE", {"PPSSPPSDL", "ppsspp", "PPSSPPQt"}}
  };
#endif
}
std::vector<fs::path> default_roots() {
  std::vector<fs::path> roots;
#ifdef _WIN32
  for (const char* variable : {"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"}) if (const auto value = environment_path(variable)) {
    roots.push_back(*value); roots.push_back(*value / "Programs");
  }
#elif defined(__APPLE__)
  roots = {"/Applications", "/usr/local/bin", "/opt/homebrew/bin"};
  if (const auto home = environment_path("HOME")) roots.push_back(*home / "Applications");
#else
  roots = {"/usr/bin", "/usr/local/bin", "/opt/bin"};
  if (const auto home = environment_path("HOME")) { roots.push_back(*home / ".local/bin"); roots.push_back(*home / "Applications"); }
#endif
  return roots;
}
void validate_options(const RunnerDiscoveryOptions& options, const std::vector<Definition>& known) {
  if (options.search_roots.size() > 128 || options.executable_overrides.size() > known.size()) throw std::runtime_error("Choose at most 128 runner search roots and seven executable overrides");
  for (const auto& root : options.search_roots) valid_path(root);
  for (const auto& [id, path] : options.executable_overrides) {
    if (std::ranges::none_of(known, [&](const auto& value) { return value.id == id; })) throw std::runtime_error("Unknown runner executable override: " + id);
    valid_path(path);
  }
}
std::optional<fs::path> find_requested(const fs::path& executable, const RunnerDiscoveryOptions& options) {
  if (executable.empty()) return {};
  if (executable.is_absolute() || executable.has_parent_path()) return usable(executable) ? std::optional{fs::weakly_canonical(executable)} : std::nullopt;
  for (const auto& root : options.search_roots) if (usable(root / executable)) return fs::weakly_canonical(root / executable);
  if (options.search_path) if (const auto found = integrations::find_executable(text(executable))) return fs::weakly_canonical(*found);
  if (options.search_defaults) for (const auto& root : default_roots()) if (usable(root / executable)) return fs::weakly_canonical(root / executable);
  return {};
}
TrackingQuality native_quality(const fs::path& executable) {
  const auto name = lower(text(executable.filename()));
  for (const auto* launcher : {"steam", "steam.exe", "steam-runtime", "steam_osx", "xdg-open", "open", "rundll32.exe", "explorer.exe", "firefox", "firefox.exe", "chromium", "google-chrome", "chrome.exe", "msedge.exe"})
    if (name == launcher) return TrackingQuality::launcher_only;
  for (const auto* shell : {"sh", "sh.exe", "bash", "bash.exe", "dash", "zsh", "fish", "cmd.exe", "powershell.exe", "pwsh", "pwsh.exe", "flatpak", "snap", "wine", "wine64", "python", "python.exe", "python3", "python3.exe", "py.exe", "node", "node.exe", "wscript.exe", "cscript.exe"})
    if (name == shell) return TrackingQuality::unknown;
  const auto extension = lower(text(executable.extension()));
  return extension == ".sh" || extension == ".bat" || extension == ".cmd" || extension == ".ps1" || extension == ".py" || extension == ".js" || extension == ".vbs" ? TrackingQuality::unknown : TrackingQuality::direct_child;
}
fs::path content_path(const fs::path& value, const fs::path& cwd, bool directory_allowed, const char* label) {
  if (value.empty()) throw std::runtime_error(std::string("Choose the ") + label + " path before launching");
  valid_path(value);
  const auto path = value.is_relative() && !cwd.empty() ? cwd / value : value;
  std::error_code error;
  if (!fs::is_regular_file(path, error) && !(directory_allowed && fs::is_directory(path, error))) throw std::runtime_error(std::string("The ") + label + " path does not exist or is not a supported file");
  return fs::weakly_canonical(fs::absolute(path));
}
std::string steam_id(const fs::path& content) {
  const auto value = text(content);
  if (value.empty() || value.size() > 10 || !std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; })) throw std::runtime_error("Steam content must be a positive numeric app id, for example 570");
  const auto id = std::stoull(value);
  if (!id || id > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("Steam app id must be between 1 and 4294967295");
  return std::to_string(id);
}
// Templates verified against each upstream CLI parser. Dolphin's --config is
// a command-line-only layer whose Save() writes nothing to emulator settings.
std::vector<std::string> defaults(const std::string& id, const fs::path& content, const fs::path& core, bool fullscreen) {
  std::vector<std::string> result;
  if (id == "rpcs3") { result = {"--no-gui"}; if (fullscreen) result.push_back("--fullscreen"); }
  else if (id == "pcsx2" || id == "duckstation") { result = {"-batch"}; if (fullscreen) result.push_back("-fullscreen"); }
  else if (id == "retroarch") { if (fullscreen) result.push_back("-f"); result.push_back("-L"); result.push_back(text(core)); }
  else if (id == "dolphin") { result = {"--batch"}; if (fullscreen) { result.push_back("--config"); result.push_back("Dolphin.Display.Fullscreen=True"); } result.push_back("--exec"); }
  else if (id == "ppsspp") { result = {"--pause-menu-exit"}; if (fullscreen) result.push_back("--fullscreen"); }
  else if (id == "steam") return {"-applaunch", steam_id(content)};
  result.push_back(text(content)); return result;
}
}
RunnerDiscoveryOptions runner_options_from_preferences(const std::map<std::string, std::string>& preferences) {
  RunnerDiscoveryOptions result;
  for (const auto& definition : definitions()) {
    const auto found = preferences.find(std::string("runner.") + definition.id + ".path");
    if (found != preferences.end() && !found->second.empty()) result.executable_overrides.emplace(definition.id, fs::path(std::u8string(found->second.begin(), found->second.end())));
  }
  return result;
}
std::vector<RunnerDescriptor> discover_runners(const RunnerDiscoveryOptions& options) {
  const auto known = definitions(); validate_options(options, known);
  auto roots = options.search_roots;
  if (options.search_defaults) { const auto system = default_roots(); roots.insert(roots.end(), system.begin(), system.end()); }
  std::vector<RunnerDescriptor> result; result.reserve(known.size());
  for (const auto& definition : known) {
    RunnerDescriptor runner{definition.id, definition.name, {}, std::string(definition.id) == "steam" ? TrackingQuality::launcher_only : TrackingQuality::direct_child, true, std::string(definition.id) == "retroarch", {}};
    std::optional<fs::path> selected;
    if (const auto override = options.executable_overrides.find(runner.id); override != options.executable_overrides.end()) selected = override->second;
    else if (options.search_defaults) selected = environment_path(definition.environment);
    if (selected) {
      if (usable(*selected)) runner.executable = fs::weakly_canonical(fs::absolute(*selected));
      else runner.message = "The configured " + runner.name + " executable is missing or not executable; choose its installed executable";
    } else {
      for (const auto& root : roots) {
        for (const auto& name : definition.names) if (usable(root / name)) { runner.executable = fs::weakly_canonical(root / name); break; }
        if (runner) break;
      }
      if (!runner && options.search_path) for (const auto& name : definition.names) {
        if (name.has_parent_path()) continue;
        if (const auto path = integrations::find_executable(text(name))) { runner.executable = fs::weakly_canonical(*path); break; }
      }
    }
    if (runner) {
      const auto quality = native_quality(runner.executable);
      if (quality != TrackingQuality::direct_child) runner.tracking = quality;
      runner.message = "Found installed executable: " + text(runner.executable);
    } else if (runner.message.empty()) runner.message = "Install " + runner.name + " or choose its executable path; OpenXMB does not install it automatically";
    result.push_back(std::move(runner));
  }
  return result;
}
LaunchPlan plan_game_launch(const Game& game, const RunnerDiscoveryOptions& options, bool fullscreen) {
  LaunchPlan plan; plan.runner = game.runner.empty() ? "native" : game.runner;
  try {
    validate_options(options, definitions()); valid_path(game.executable); valid_path(game.working_directory); valid_arguments(game.arguments);
    if (!game.working_directory.empty() && !fs::is_directory(game.working_directory)) throw std::runtime_error("The working directory does not exist; choose an existing directory");
    if (plan.runner == "native") {
      if (game.executable.empty()) throw std::runtime_error("Choose this game's executable before launching");
      const auto executable = find_requested(game.executable, options);
      if (!executable) throw std::runtime_error("The game's executable was not found; choose an installed executable or add it to PATH");
      plan.arguments = {text(game.executable.has_parent_path() ? game.executable : *executable)}; plan.arguments.insert(plan.arguments.end(), game.arguments.begin(), game.arguments.end());
      plan.working_directory = game.working_directory; plan.tracking = native_quality(game.executable);
    } else {
      const auto runners = discover_runners(options);
      const auto found = std::ranges::find_if(runners, [&](const auto& runner) { return runner.id == plan.runner; });
      if (found == runners.end()) throw std::runtime_error("Unknown runner; choose RPCS3, PCSX2, Steam, RetroArch, Dolphin, DuckStation or PPSSPP");
      auto runner = *found;
      const bool override = options.executable_overrides.contains(plan.runner);
      if (!game.executable.empty() && !override) {
        if (const auto selected = find_requested(game.executable, options)) { runner.executable = *selected; runner.message.clear(); }
        else if (game.executable.has_parent_path() || std::ranges::none_of(definitions(), [&](const auto& definition) { return definition.id == plan.runner && std::ranges::any_of(definition.names, [&](const auto& name) { return name == game.executable; }); }))
          throw std::runtime_error("The selected runner executable was not found; choose its installed executable");
      }
      if (!runner) throw std::runtime_error(runner.message);
      const auto content = plan.runner == "steam" ? game.content : content_path(game.content, game.working_directory, plan.runner == "rpcs3", "content");
      fs::path core;
      if (runner.requires_core) core = content_path(game.core, game.working_directory, false, "libretro core");
      const auto automatic = defaults(plan.runner, content, core, fullscreen);
      const auto& arguments = game.arguments.empty() ? automatic : game.arguments;
      if (plan.runner == "steam" && !game.arguments.empty()) {
        const auto launch = std::ranges::find(game.arguments, "-applaunch");
        if (launch == game.arguments.end() || std::next(launch) == game.arguments.end() || *std::next(launch) != steam_id(content)) throw std::runtime_error("Explicit Steam arguments must launch the same validated app id with -applaunch");
      }
      plan.arguments = {text(runner.executable)}; plan.arguments.insert(plan.arguments.end(), arguments.begin(), arguments.end());
      plan.working_directory = game.working_directory.empty() ? runner.executable.parent_path() : game.working_directory;
      plan.tracking = plan.runner == "steam" ? TrackingQuality::launcher_only : runner.tracking;
      const bool imported_rpcs3 = plan.runner == "rpcs3" && game.arguments == std::vector<std::string>{"--no-gui", text(content)};
      if (plan.runner != "steam" && !game.arguments.empty() && game.arguments != automatic && !imported_rpcs3) plan.tracking = TrackingQuality::unknown;
    }
    valid_arguments(plan.arguments); plan.success = true;
    plan.message = plan.tracking == TrackingQuality::launcher_only ? "Launcher-only observation; the game's process and playtime are not tracked" : plan.tracking == TrackingQuality::unknown ? "Literal launch plan ready; application lifetime is unverified" : "Literal launch plan ready; direct child process will be observed";
  } catch (const std::exception& error) { plan.success = false; plan.arguments.clear(); plan.message = error.what(); }
  return plan;
}
} // namespace openxmb::desktop
