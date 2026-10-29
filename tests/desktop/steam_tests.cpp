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
void put32(std::string& bytes,std::uint32_t value) {for(unsigned i=0;i<4;++i)bytes+=static_cast<char>((value>>(8*i))&255);}
void put64(std::string& bytes,std::uint64_t value) {for(unsigned i=0;i<8;++i)bytes+=static_cast<char>((value>>(8*i))&255);}
std::string binary_appinfo(const std::vector<std::pair<std::uint32_t,std::string>>& payloads,
                          const std::vector<std::string>& keys={"appinfo","common","type"}) {
  std::string records;
  for(const auto& [id,payload]:payloads) {
    put32(records,id);put32(records,static_cast<std::uint32_t>(60+payload.size()));records.append(60,'\0');records+=payload;
  }
  put32(records,0);
  std::string result;put32(result,0x07564429);put32(result,1);put64(result,16+records.size());result+=records;
  put32(result,static_cast<std::uint32_t>(keys.size()));for(const auto& key:keys){result+=key;result+='\0';}return result;
}
std::string appinfo(const std::vector<std::pair<std::uint32_t,std::string>>& types) {
  std::vector<std::pair<std::uint32_t,std::string>> payloads;
  for(const auto& [id,type]:types) {
    std::string payload;payload+='\0';put32(payload,0);payload+='\0';put32(payload,1);
    payload+='\1';put32(payload,2);payload+=type;payload+='\0';payload.append(3,'\10');
    payloads.emplace_back(id,std::move(payload));
  }
  return binary_appinfo(payloads);
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
  installed(client, "300", "Portrait-only title", "Game");
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
void product_types(const fs::path& root) {
  installed(root,"228980","Steamworks Common Redistributables","Steamworks Shared");
  installed(root,"4099820","Ubisoft Connect Services","Ubisoft Connect Services");
  installed(root,"2225070","Trackmania","Trackmania");
  installed(root,"101","Unfamiliar framework product","Support");
  installed(root,"102","Game demo","Demo");
  installed(root,"103","Ubisoft named game with no appinfo","Unknown");
  installed(root,"104","A future product type","Future");
  const auto cache=root/"appcache/appinfo.vdf";
  const auto bytes=appinfo({{228980,"Tool"},{4099820,"Tool"},{2225070,"Game"},{101,"Tool"},{102,"Demo"},{104,"FutureType"}});
  file(cache,bytes);const auto before=inventory(root);
  const auto discovered=discover_steam_games({root});
  require(discovered.size()==4,"local appinfo must exclude support tools while preserving games demos and unknown types");
  (void)by_id(discovered,"2225070");(void)by_id(discovered,"102");(void)by_id(discovered,"103");(void)by_id(discovered,"104");
  Game support;support.runner="steam";support.content="101";support.working_directory=root/"steamapps/common/Support";
  require(!steam_game_is_playable(support),"stored provider support records use the same product type predicate");
  auto native=support;native.runner="native";native.title="Ubisoft Connect Services";
  require(steam_game_is_playable(native),"explicit native imports are unaffected by provider support classification");
  require(inventory(root)==before,"product classification must not change source metadata or artwork");
  const auto previous_write=fs::last_write_time(cache);
  file(cache,appinfo({{101,"Game"}}));fs::last_write_time(cache,previous_write+std::chrono::seconds(1));
  require(steam_game_is_playable(support),"appinfo replacement invalidates cached product classification");
  file(cache,bytes);
  const auto fallback=[&](const std::string& malformed) {
    file(cache,malformed);const auto found=discover_steam_games({root});
    require(found.size()==5,"malformed/unsupported optional metadata must preserve unknown installed games while known support IDs stay excluded");
    (void)by_id(found,"101");(void)by_id(found,"2225070");
  };
  auto bad=bytes;bad[0]='\x28';fallback(bad);
  bad=bytes;std::fill(bad.begin()+8,bad.begin()+16,'\xff');fallback(bad);
  bad=bytes;std::fill(bad.begin()+20,bad.begin()+24,'\xff');fallback(bad);
  bad=bytes;bad.resize(15);fallback(bad);
  fallback(appinfo({{101,"Tool"},{101,"Game"}}));
  // Malformed one-record payloads cannot classify a product through an
  // out-of-bounds key index, a truncated scalar, or an empty product type.
  for(const auto mutate:{0,1,2}) {
    auto record=appinfo({{101,"Tool"}});
    if(mutate==0)std::fill(record.begin()+85,record.begin()+89,'\xff');
    else if(mutate==1)record[94]='\2';
    else record[99]='\0';
    fallback(record);
  }
  const auto field=[](unsigned kind,std::uint32_t key,std::string value) {
    std::string result(1,static_cast<char>(kind));put32(result,key);result+=value;return result;
  };
  const auto object=[&](std::uint32_t key,std::string fields) {fields+='\10';return field(0,key,std::move(fields));};
  const auto tool=field(1,2,std::string("Tool")+'\0');
  const auto common=object(1,tool),empty_common=object(1,{}),empty_app=object(0,{});
  const auto invalid=[&](std::string fields,const std::vector<std::string>& keys={"appinfo","common","type"}) {
    fields+='\10';fallback(binary_appinfo({{101,std::move(fields)}},keys));
  };
  invalid(object(0,object(1,tool+field(2,2,std::string(4,'\0')))));
  invalid(object(0,object(1,field(2,2,std::string(4,'\0'))+tool)));
  invalid(object(0,object(1,tool+object(2,{}))));
  invalid(object(0,common+empty_common));
  invalid(object(0,empty_common+common));
  invalid(object(0,common)+empty_app);
  invalid(empty_app+object(0,common));
  invalid(object(0,field(1,1,std::string("ignored")+'\0')+common));
  invalid(object(0,object(1,tool+field(2,3,std::string(4,'\0')))),{"appinfo","common","type","TYPE"});
  invalid(object(0,common+object(3,{})),{"appinfo","common","type","COMMON"});
  invalid(object(0,common)+object(3,{}),{"appinfo","common","type","APPINFO"});
}
}
int main() {
  const auto root = fs::weakly_canonical(fs::temp_directory_path()) / ("openxmb-steam-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try { normal(root / "normal"); malformed(root / "malformed"); asset_roles(root / "assets"); product_types(root / "types"); fs::remove_all(root); std::cout << "Steam registered-library, installed-manifest and product-type checks passed\n"; }
  catch (const std::exception& error) { fs::remove_all(root); std::cerr << error.what() << '\n'; return 1; }
}
