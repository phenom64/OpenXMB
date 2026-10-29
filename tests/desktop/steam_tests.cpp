#include "openxmb/desktop/steam.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace openxmb::desktop;
namespace {
void require(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }
std::string text(const fs::path& path) { const auto value = path.u8string(); return {value.begin(), value.end()}; }
std::string quote(const std::string& value) {
  std::string result = "\"";
  for (const auto c : value) { if (c == '\\' || c == '"') result += '\\'; result += c; }
  return result + '"';
}
void file(const fs::path& path, const std::string& value) { fs::create_directories(path.parent_path()); std::ofstream(path, std::ios::binary) << value; }
std::string manifest(const std::string& id, const std::string& name, const std::string& directory, const std::string& state = "4", const std::string& extra = {}) {
  return "\"AppState\"\n{\n\"appid\" " + quote(id) + "\n\"name\" " + quote(name) + "\n\"StateFlags\" " + quote(state) + "\n\"installdir\" " + quote(directory) + "\n" + extra + "\n\"InstalledDepots\" { \"123\" { \"manifest\" \"123456789\" } }\n}\n";
}
void installed(const fs::path& library, const std::string& id, const std::string& name, const std::string& directory) {
  fs::create_directories(library / "steamapps/common" / fs::path(std::u8string(directory.begin(), directory.end())));
  file(library / "steamapps" / ("appmanifest_" + id + ".acf"), manifest(id, name, directory));
}
const Game& by_id(const GameDiscovery& games, const std::string& id) {
  const auto found = std::ranges::find_if(games, [&](const auto& candidate) { return text(candidate.game.content) == id; });
  require(found != games.end(), "expected Steam title was not discovered"); return found->game;
}
std::map<std::string, std::string> inventory(const fs::path& root) {
  std::map<std::string, std::string> result;
  for (const auto& item : fs::recursive_directory_iterator(root)) if (item.is_regular_file()) {
    std::ifstream input(item.path(), std::ios::binary); result[text(fs::relative(item.path(), root))] = {std::istreambuf_iterator<char>(input), {}};
  }
  return result;
}
void normal(const fs::path& root) {
  const auto client = root / "Steam client";
  const auto library = root / fs::path(std::u8string(u8"Player's library \u6771\u4eac"));
  const auto unregistered = root / "unregistered";
  installed(client, "100", "First installed title", "First game");
  const std::string unicode = reinterpret_cast<const char*>(u8"Quoted \"Deluxe\" \\ \u6771\u4eac");
  installed(library, "200", unicode, "Unicode Game");
  installed(library, "100", "Duplicate secondary copy", "Copy");
  installed(client, "300", "Steamworks Common Redistributables", "Tool");
  installed(unregistered, "900", "Must not crawl this title", "Hidden");
  file(client / "steamapps/libraryfolders.vdf", "\xef\xbb\xbf\"libraryfolders\"\n{\n// Steam serializes escaped path scalars\n\"0\" { \"path\" " + quote(text(client)) + " \"apps\" { \"100\" \"123\" } }\n\"1\" { \"path\" " + quote(text(library)) + " \"label\" \"\" }\n\"2\" { \"path\" " + quote(text(root / "missing library")) + " }\n\"3\" { \"path\" \"relative-library\" }\n\"4\" { \"path\" \"//unreachable-openxmb.invalid/share\" }\n}\n");
  // Registered secondary libraries do not grant another transitive registry.
  file(library / "steamapps/libraryfolders.vdf", "\"libraryfolders\" { \"0\" { \"path\" " + quote(text(unregistered)) + " } }");
  const auto cache = client / "appcache/librarycache";
  file(cache / "100_header.jpg", "header fixture"); file(cache / "100_library_hero.jpg", "hero fixture");
  const auto hash = cache / "200/0123456789abcdef0123456789abcdef01234567";
  file(hash / "library_header.jpg", "current cache header fixture"); file(hash / "library_hero.jpg", "current cache hero fixture");
  file(cache / "300_library_600x900.jpg", "portrait fixture");
  file(cache / "300/0123456789abcdef0123456789abcdef01234567/library_600x900.jpg", "current portrait fixture");
  const auto before = inventory(root);
  const auto executable = root / "steam.exe";
  const auto games = discover_steam_games({client}, executable);
  require(games.size() == 3, "Steam registry discovery must import unique installed app ids without recursively following secondary registries");
  const auto& first = by_id(games, "100");
  require(first.title == "First installed title" && first.runner == "steam" && first.platform == "Steam" && first.arguments.empty() && first.game_directory.empty() && first.executable == executable, "Steam identity/planner fields are incorrect");
  require(first.working_directory == fs::weakly_canonical(client / "steamapps/common/First game"), "installed directory was not resolved under the proper library");
  require(first.icon == cache / "100_header.jpg" && first.background == cache / "100_library_hero.jpg", "legacy native horizontal header and hero assets were not selected");
  const auto& second = by_id(games, "200");
  require(second.title == unicode && second.icon == hash / "library_header.jpg" && second.background == hash / "library_hero.jpg", "quoted UTF8 metadata or current hashed-directory role-named assets were not imported correctly");
  require(by_id(games, "300").icon.empty() && by_id(games, "300").background.empty(), "portrait Steam library art must not be assigned to icon or background roles");
  require(inventory(root) == before, "Steam discovery changed files or created output inside its source roots");
  require(discover_steam_games({client, client}).size() == 3, "explicit and registered duplicate library paths/app ids were not deduplicated");
}
void malformed(const fs::path& root) {
  const auto client = root / "client"; const auto external = root / "external";
  installed(client, "10", "Valid local", "Local"); installed(external, "20", "Valid external", "External");
  const auto registry = client / "steamapps/libraryfolders.vdf";
  const auto valid_prefix = "\"libraryfolders\" { \"0\" { \"path\" " + quote(text(external)) + " } ";
  for (const auto& suffix : {std::string{}, std::string("\"bad\" \"unterminated"), std::string("\"1\" { \"path\" \"bad\\qescape\" } }"), std::string("\"0\" { \"path\" \"/duplicate\" } }"), std::string("#include \"other.vdf\" }"), std::string("\"missing-value\"}")}) {
    file(registry, valid_prefix + suffix);
    const auto found = discover_steam_games({client});
    require(found.size() == 1 && text(found.front().game.content) == "10", "malformed registry must fail closed while independent local manifests remain discoverable");
  }
  { std::ofstream output(registry, std::ios::binary); output.seekp(4 * 1024 * 1024); output.put('x'); }
  require(discover_steam_games({client}).size() == 1, "oversized library registry was accepted");
  fs::remove(registry);
  file(client / "config/libraryfolders.vdf", "LibraryFolders { 0 " + quote(text(external)) + " }");
  require(discover_steam_games({client}).size() == 2, "legacy scalar libraryfolders layout was not supported when the modern registry is absent");
  file(client / "steamapps/appmanifest_21.acf", manifest("22", "Mismatching id", "Local"));
  file(client / "steamapps/appmanifest_23.acf", manifest("23", "Partial download", "Local", "2"));
  file(client / "steamapps/appmanifest_24.acf", manifest("24", "False fully installed", "Local", "4", "\"BytesToDownload\" \"100\" \"BytesDownloaded\" \"99\""));
  file(client / "steamapps/appmanifest_25.acf", manifest("25", "Partial stage", "Local", "4", "\"BytesToStage\" \"100\" \"BytesStaged\" \"0\""));
  file(client / "steamapps/appmanifest_26.acf", manifest("26", "Missing folder", "Missing"));
  file(client / "steamapps/appmanifest_27.acf", manifest("27", "Traversal", "../outside"));
  file(client / "steamapps/appmanifest_28.acf", manifest("28", "Windows traversal", "..\\outside"));
  file(client / "steamapps/appmanifest_29.acf", manifest("29", "Truncated", "Local") + "\"trailing\"");
  file(client / "steamapps/appmanifest_30.acf", manifest("30", "Wrong target build", "Local", "4", "\"buildid\" \"1\" \"TargetBuildID\" \"2\""));
  file(client / "steamapps/appmanifest_31.acf", manifest("31", "Queued/partial flags", "Local", "1028"));
  file(client / "steamapps/appmanifest_32.acf", manifest("32", "Invalid counter", "Local", "4", "\"BytesToDownload\" \"-1\""));
  file(client / "steamapps/appmanifest_4294967296.acf", manifest("4294967296", "Oversized id", "Local"));
  { std::ofstream output(client / "steamapps/appmanifest_33.acf", std::ios::binary); output.seekp(1024 * 1024); output.put('x'); }
  std::string deep = "\"AppState\" { "; for (unsigned i = 0; i < 18; ++i) deep += "\"deep\" { "; for (unsigned i = 0; i < 19; ++i) deep += " } ";
  file(client / "steamapps/appmanifest_34.acf", deep);
  file(client / "steamapps/appmanifest_35.acf", manifest("35", "Malformed download counter object", "Local", "4", "\"BytesToDownload\" { \"pending\" \"100\" }"));
  file(client / "steamapps/appmanifest_36.acf", manifest("36", "Malformed target build object", "Local", "4", "\"TargetBuildID\" { \"pending\" \"100\" }"));
  require(discover_steam_games({client}).size() == 2, "corrupt/incomplete/missing/traversing app manifests were imported as installed titles");
  bool rejected = false;
  try { (void)discover_steam_games(std::vector<fs::path>(65, client)); } catch (const std::exception&) { rejected = true; }
  require(rejected, "unbounded explicit installation root input must be rejected");
}
void asset_roles(const fs::path& root) {
  installed(root, "10", "Native icon only", "Game");
  file(root / "appcache/librarycache/10_icon.png", "native small icon fixture");
  const auto found = discover_steam_games({root});
  require(found.size() == 1 && !found.front().game.icon.empty() && found.front().game.background.empty(), "a native square icon must not be stretched into the background role");
}
}
int main() {
  const auto root = fs::weakly_canonical(fs::temp_directory_path()) / ("openxmb-steam-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try { normal(root / "normal"); malformed(root / "malformed"); asset_roles(root / "assets"); fs::remove_all(root); std::cout << "Steam registered-library and installed-manifest checks passed\n"; }
  catch (const std::exception& error) { fs::remove_all(root); std::cerr << error.what() << '\n'; return 1; }
}
