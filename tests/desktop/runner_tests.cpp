#include "openxmb/desktop/runners.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace openxmb::desktop;
namespace {
void require(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }
std::string text(const fs::path& path) { const auto value = path.u8string(); return {value.begin(), value.end()}; }
void file(const fs::path& path, bool executable = false) {
  fs::create_directories(path.parent_path()); std::ofstream(path) << "inert fixture; discovery must never execute this";
  if (executable) fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);
}
const RunnerDescriptor& by_id(const std::vector<RunnerDescriptor>& runners, const std::string& id) {
  const auto found = std::ranges::find_if(runners, [&](const auto& runner) { return runner.id == id; });
  require(found != runners.end(), "known runner descriptor missing"); return *found;
}
RunnerDiscoveryOptions isolated() { RunnerDiscoveryOptions result; result.search_path = false; result.search_defaults = false; return result; }
void discovery(const fs::path& root) {
  auto options = isolated();
  const auto missing = discover_runners(options);
  require(missing.size() == 7 && missing.front().id == "rpcs3", "discovery must list seven inspectable runners with RPCS3 first");
  require(std::ranges::none_of(missing, [](const auto& runner) { return bool(runner) || runner.message.empty(); }), "missing runners need actionable messages without host dependency");
  for (const auto& runner : missing) {
    const auto executable = root / (runner.id + " fixture.exe"); file(executable, true); options.executable_overrides[runner.id] = executable;
  }
  const auto found = discover_runners(options);
  require(std::ranges::all_of(found, [](const auto& runner) { return bool(runner); }), "explicit executable overrides were not discovered");
  require(by_id(found, "steam").tracking == TrackingQuality::launcher_only && by_id(found, "retroarch").requires_core, "runner capabilities/tracking quality are incorrect");
  options.executable_overrides["rpcs3"] = root / "missing-rpcs3";
  require(!by_id(discover_runners(options), "rpcs3"), "broken explicit executable override must not silently fall back");
  options = isolated(); options.search_roots.push_back(root / "local-bin");
#ifdef _WIN32
  file(root / "local-bin/pcsx2-qt.exe", true); file(root / "local-bin/nested/duckstation-qt-x64-ReleaseLTCG.exe", true);
#else
  file(root / "local-bin/pcsx2-qt", true); file(root / "local-bin/nested/duckstation-qt", true);
#endif
  const auto local = discover_runners(options);
  require(bool(by_id(local, "pcsx2")) && !by_id(local, "duckstation"), "search roots must probe known paths without recursive crawling");
  options.search_roots.resize(129, root); bool rejected = false;
  try { (void)discover_runners(options); } catch (const std::exception&) { rejected = true; }
  require(rejected, "oversized runner search roots must be rejected explicitly");
  const auto unicode = root / fs::path(std::u8string(u8"RPCS3 \u6771\u4eac.exe")); file(unicode, true);
  auto preferences = runner_options_from_preferences({{"runner.rpcs3.path", text(unicode)}, {"runner.pcsx2.path", ""}, {"runner.steam.path", text(root / "missing-steam")}, {"runner.unknown.path", "/ignored"}, {"unrelated", "ignored"}});
  require(preferences.search_path && preferences.search_defaults && preferences.executable_overrides.size() == 2 && preferences.executable_overrides.at("rpcs3") == unicode, "runner preferences must map only known nonempty UTF8 path settings with default discovery options");
  preferences.search_path = false; preferences.search_defaults = false;
  require(bool(by_id(discover_runners(preferences), "rpcs3")) && !by_id(discover_runners(preferences), "steam"), "preferences should preserve usable Unicode paths and report nonexistent paths through discovery");
  const auto invalid = runner_options_from_preferences({{"runner.rpcs3.path", std::string("bad\0path", 8)}});
  rejected = false; try { (void)discover_runners(invalid); } catch (const std::exception&) { rejected = true; }
  require(rejected, "invalid runner preference paths must be rejected by discovery without truncation");
}
void plans(const fs::path& root) {
  auto options = isolated();
  const std::vector<std::string> ids{"rpcs3", "pcsx2", "steam", "retroarch", "dolphin", "duckstation", "ppsspp"};
  for (const auto& id : ids) { const auto executable = root / (id + ".exe"); file(executable, true); options.executable_overrides[id] = executable; }
  const auto content = root / fs::path(std::u8string(u8"Game ; $(touch injected) \u6771\u4eac.iso")); file(content);
  const auto core = root / fs::path(std::u8string(u8"Core \u6771\u4eac.so")); file(core);
  const std::map<std::string, std::vector<std::string>> expected{
    {"rpcs3", {"--no-gui", "--fullscreen", text(content)}},
    {"pcsx2", {"-batch", "-fullscreen", text(content)}},
    {"retroarch", {"-f", "-L", text(core), text(content)}},
    {"dolphin", {"--batch", "--config", "Dolphin.Display.Fullscreen=True", "--exec", text(content)}},
    {"duckstation", {"-batch", "-fullscreen", text(content)}},
    {"ppsspp", {"--pause-menu-exit", "--fullscreen", text(content)}}
  };
  for (const auto& [id, args] : expected) {
    Game game; game.runner = id; game.content = content; game.core = core;
    auto plan = plan_game_launch(game, options);
    auto argv = args; argv.insert(argv.begin(), text(options.executable_overrides.at(id)));
    require(bool(plan) && plan.arguments == argv && plan.tracking == TrackingQuality::direct_child, "default runner launch argv/quality differs from verified native template");
    require(plan.working_directory == root, "default runner working directory should be its installation directory");
  }
  Game game; game.runner = "retroarch"; game.content = content;
  require(!plan_game_launch(game, options) && plan_game_launch(game, options).message.find("core") != std::string::npos, "missing libretro core needs an actionable error");
  game.core = root / "missing.so"; require(!plan_game_launch(game, options), "missing libretro core file was accepted");
  game.runner = "pcsx2"; game.content.clear(); require(!plan_game_launch(game, options), "missing content was accepted");
  game.content = root / "missing.iso"; require(!plan_game_launch(game, options), "nonexistent content was accepted");
  game.content = content; game.working_directory = root / "missing-cwd"; require(!plan_game_launch(game, options), "invalid working directory was accepted");
  game.working_directory.clear(); game.runner = "made-up"; require(!plan_game_launch(game, options), "unknown runner was silently treated as native");
  game.runner = "steam"; game.content = "570";
  const auto steam = plan_game_launch(game, options);
  require(bool(steam) && steam.arguments == std::vector<std::string>{text(options.executable_overrides.at("steam")), "-silent", "-applaunch", "570"} && steam.tracking == TrackingQuality::launcher_only, "Steam app id plan must preserve forwarding semantics");
  for (const char* invalid : {"", "0", "-1", "570;touch injected", "4294967296", "steam://run/570"}) { game.content = invalid; require(!plan_game_launch(game, options), "invalid Steam app id was accepted"); }
  game = {}; game.executable = root / "native game.exe"; file(game.executable, true); game.working_directory = root;
  require(plan_game_launch(game, options).tracking == TrackingQuality::direct_child, "ordinary native binary should expose direct-child observation");
  game.arguments = {"", "literal ; $(touch injected)", "a\\\"b", std::string(reinterpret_cast<const char*>(u8"\u6771\u4eac"))};
  const auto native = plan_game_launch(game, options);
  auto native_argv = game.arguments; native_argv.insert(native_argv.begin(), text(game.executable));
  require(bool(native) && native.arguments == native_argv && native.working_directory == root && native.tracking == TrackingQuality::direct_child, "ordinary native binary argv/cwd must remain literal and expose only direct-child lifetime");
  game.arguments = {std::string("bad\0argument", 12)}; require(!plan_game_launch(game, options), "NUL launch argument was accepted");
  game = {}; game.executable = "native-from-root.exe"; file(root / game.executable, true);
  auto native_options = isolated(); native_options.search_roots.push_back(root);
  require(plan_game_launch(game, native_options).arguments == std::vector<std::string>{text(root / game.executable)}, "native discovery through an explicit search root must produce an executable absolute argv path");
  game = {}; game.executable = root / "sh"; file(game.executable, true); game.arguments = {"-c", "literal fixture"};
  require(plan_game_launch(game, options).tracking == TrackingQuality::unknown, "native command shells must not claim application lifetime");
  game.executable = options.executable_overrides.at("steam"); game.arguments = {"-applaunch", "570"};
  require(plan_game_launch(game, options).tracking == TrackingQuality::launcher_only, "native Steam entry must retain launcher-only tracking");
  game = {}; game.runner = "rpcs3"; game.executable = options.executable_overrides.at("rpcs3"); game.content = content; game.arguments = {"--no-gui", text(content)};
  const auto imported = plan_game_launch(game, options);
  require(bool(imported) && imported.arguments == std::vector<std::string>{text(game.executable), "--no-gui", text(content)} && imported.tracking == TrackingQuality::direct_child, "existing imported RPCS3 argv must remain exact without duplicate flags or lost tracking");
  game.arguments = {"custom opaque args", text(content)}; require(plan_game_launch(game, options).tracking == TrackingQuality::unknown, "opaque custom emulator argv was treated as verified child lifetime");
  options.executable_overrides["rpcs3"] = root / "missing"; require(!plan_game_launch(game, options), "missing executable override must produce an error");
  require(!fs::exists(root / "injected"), "planning executed content or shell text");
}
}
int main() {
  const auto root = fs::weakly_canonical(fs::temp_directory_path()) / ("openxmb-runners-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try { discovery(root / "discovery"); plans(root / "plans"); fs::remove_all(root); std::cout << "Runner discovery and literal launch-plan checks passed\n"; }
  catch (const std::exception& error) { fs::remove_all(root); std::cerr << error.what() << '\n'; return 1; }
}
