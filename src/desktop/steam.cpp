#include "openxmb/desktop/steam.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace openxmb::desktop {
namespace {
namespace fs = std::filesystem;
std::string text(const fs::path& path) { const auto value = path.u8string(); return {value.begin(), value.end()}; }
fs::path path_from_text(const std::string& value) { return fs::path(std::u8string(value.begin(), value.end())); }
std::string lower(std::string value) { for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a'); return value; }
bool safe_text(const std::string& value, std::size_t limit, bool nonempty = true) {
  return (!nonempty || !value.empty()) && value.size() <= limit && std::ranges::none_of(value, [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}
struct ScanLimit : std::runtime_error { using std::runtime_error::runtime_error; };
struct Budget { std::uint64_t bytes{128 * 1024 * 1024}; std::size_t entries{}, manifests{}, asset_entries{}; };
std::string read_text(const fs::path& path, std::size_t maximum, Budget& budget) {
  std::error_code error;
  if (!fs::is_regular_file(path, error)) throw std::runtime_error("Steam metadata is not a regular file");
  const auto size = fs::file_size(path);
  if (size > maximum) throw std::runtime_error("Steam metadata exceeds its file limit");
  if (size > budget.bytes) throw ScanLimit("Steam metadata scan exceeded 128 MiB; choose fewer installation roots");
  budget.bytes -= size;
  std::ifstream input(path, std::ios::binary); std::string bytes(static_cast<std::size_t>(size) + 1, '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (input.bad() || static_cast<std::uint64_t>(input.gcount()) != size) throw std::runtime_error("Steam metadata changed or could not be read completely");
  bytes.resize(static_cast<std::size_t>(size)); (void)nlohmann::json(bytes).dump(); return bytes;
}
struct Node {
  bool object{};
  std::string scalar;
  std::map<std::string, Node> fields;
  const Node* get(const std::string& key) const { const auto found = fields.find(lower(key)); return found == fields.end() ? nullptr : &found->second; }
  const std::string* value(const std::string& key) const { const auto* child = get(key); return child && !child->object ? &child->scalar : nullptr; }
};
// KeyValues text subset: quoted/unquoted strings, braces and // comments.
// Includes, conditionals, duplicate keys and unknown escapes fail closed;
// no text in a VDF can cause another arbitrary file to be loaded by this parser.
class KeyValues {
public:
  explicit KeyValues(std::string_view input, std::size_t maximum_nodes) : input_(input), maximum_nodes_(maximum_nodes) { if (input_.starts_with("\xef\xbb\xbf")) at_ = 3; }
  Node parse() { auto result = object(0, false); whitespace(); if (at_ != input_.size()) throw std::runtime_error("Trailing KeyValues data"); return result; }
private:
  std::string_view input_;
  std::size_t at_{}, nodes_{}, maximum_nodes_;
  void whitespace() {
    for (;;) {
      while (at_ < input_.size() && (input_[at_] == ' ' || input_[at_] == '\t' || input_[at_] == '\r' || input_[at_] == '\n')) ++at_;
      if (input_.substr(at_).starts_with("//")) { const auto end = input_.find('\n', at_); at_ = end == std::string_view::npos ? input_.size() : end + 1; }
      else return;
    }
  }
  std::string token() {
    whitespace(); if (at_ == input_.size()) throw std::runtime_error("Truncated KeyValues token");
    std::string result;
    const bool quoted = input_[at_] == '"';
    if (quoted) {
      ++at_; bool closed = false;
      while (at_ < input_.size()) {
        auto c = input_[at_++];
        if (c == '"') { closed = true; break; }
        if (c == '\\') {
          if (at_ == input_.size()) throw std::runtime_error("Truncated KeyValues escape");
          c = input_[at_++];
          switch (c) { case '\\': case '"': break; case 'n': c = '\n'; break; case 'r': c = '\r'; break; case 't': c = '\t'; break; default: throw std::runtime_error("Unsupported KeyValues escape"); }
        }
        if (c == '\0') throw std::runtime_error("NUL in KeyValues token");
        result += c; if (result.size() > 32768) throw std::runtime_error("Oversized KeyValues token");
      }
      if (!closed) throw std::runtime_error("Unterminated KeyValues quote");
    } else {
      const auto start = at_;
      while (at_ < input_.size() && input_[at_] != ' ' && input_[at_] != '\t' && input_[at_] != '\r' && input_[at_] != '\n' && input_[at_] != '{' && input_[at_] != '}' && input_[at_] != '"') {
        if (input_[at_] == '\0') throw std::runtime_error("NUL in KeyValues token");
        if (++at_ - start > 32768) throw std::runtime_error("Oversized KeyValues token");
      }
      result = input_.substr(start, at_ - start);
    }
    if (result.empty() && !quoted) throw std::runtime_error("Invalid KeyValues token");
    if (!result.empty() && (result.front() == '#' || result.front() == '[' || result.front() == ']')) throw std::runtime_error("KeyValues directives/conditionals are unsupported");
    return result;
  }
  Node object(unsigned depth, bool braced) {
    if (depth > 16) throw std::runtime_error("KeyValues nesting exceeds 16 levels");
    Node result; result.object = true;
    for (;;) {
      whitespace();
      if (at_ == input_.size()) { if (braced) throw std::runtime_error("Truncated KeyValues object"); return result; }
      if (input_[at_] == '}') { if (!braced) throw std::runtime_error("Unexpected KeyValues closing brace"); ++at_; return result; }
      const auto key = lower(token());
      if (key.empty() || ++nodes_ > maximum_nodes_) throw std::runtime_error("Invalid or excessive KeyValues keys");
      whitespace(); Node value;
      if (at_ < input_.size() && input_[at_] == '{') { ++at_; value = object(depth + 1, true); }
      else value.scalar = token();
      if (!result.fields.emplace(key, std::move(value)).second) throw std::runtime_error("Duplicate KeyValues keys");
    }
  }
};
std::optional<std::uint64_t> number(const std::string& value) {
  if (value.empty() || value.size() > 20 || !std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; })) return {};
  std::uint64_t result{}; const auto converted = std::from_chars(value.data(), value.data() + value.size(), result);
  return converted.ec == std::errc{} && converted.ptr == value.data() + value.size() ? std::optional{result} : std::nullopt;
}
std::optional<std::string> app_id(const std::string& value) {
  const auto parsed = number(value);
  if (!parsed || !*parsed || *parsed > std::numeric_limits<std::uint32_t>::max()) return {};
  return std::to_string(*parsed);
}
bool network_path(const fs::path& path) {
  const auto value = text(path);
  if (value.starts_with("//") || value.starts_with("\\\\") || value.find("://") != std::string::npos) return true;
#ifdef _WIN32
  if (!path.root_path().empty() && GetDriveTypeW(path.root_path().c_str()) == DRIVE_REMOTE) return true;
#endif
  return false;
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
#ifdef _WIN32
std::optional<fs::path> registry_path(HKEY key, const wchar_t* subkey, const wchar_t* name) {
  std::wstring buffer(32768, L'\0'); DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
  if (RegGetValueW(key, subkey, name, RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return {};
  buffer.resize(bytes / sizeof(wchar_t) - 1); return buffer.empty() ? std::nullopt : std::optional{fs::path(buffer)};
}
#endif
std::vector<fs::path> default_roots() {
  std::vector<fs::path> result;
#ifdef _WIN32
  if (const auto path = registry_path(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath")) result.push_back(*path);
  if (const auto path = registry_path(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath")) result.push_back(*path);
  for (const auto* name : {"ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA"}) if (const auto path = environment_path(name)) result.push_back(*path / "Steam");
#elif defined(__APPLE__)
  if (const auto home = environment_path("HOME")) result.push_back(*home / "Library/Application Support/Steam");
#else
  if (const auto data = environment_path("XDG_DATA_HOME")) result.push_back(*data / "Steam");
  if (const auto home = environment_path("HOME")) for (const auto* suffix : {".local/share/Steam", ".steam/steam", ".steam/root", ".var/app/com.valvesoftware.Steam/.local/share/Steam"}) result.push_back(*home / suffix);
#endif
  return result;
}
std::vector<fs::path> registered_paths(const fs::path& root, Budget& budget) {
  auto file = root / "steamapps/libraryfolders.vdf";
  if (!fs::exists(file)) file = root / "config/libraryfolders.vdf";
  const auto bytes = read_text(file, 4 * 1024 * 1024, budget);
  const auto document = KeyValues(bytes, 65536).parse(); const auto* folders = document.get("libraryfolders");
  if (document.fields.size() != 1 || !folders || !folders->object) throw std::runtime_error("Invalid Steam library registry root");
  std::vector<fs::path> paths;
  for (const auto& [index, entry] : folders->fields) {
    if (!number(index)) continue;
    const auto* value = entry.object ? entry.value("path") : &entry.scalar;
    if (!value || !safe_text(*value, 32768)) continue;
    const auto path = path_from_text(*value);
    if (!path.is_absolute() || network_path(path)) continue;
    if (paths.size() >= 64) throw std::runtime_error("Steam registry exceeds 64 library roots");
    paths.push_back(path);
  }
  return paths;
}
bool complete_counter(const Node& state, const char* total_key, const char* done_key) {
  const auto* total_text = state.value(total_key); const auto* done_text = state.value(done_key);
  if ((state.get(total_key) && !total_text) || (state.get(done_key) && !done_text)) return false;
  const auto total = total_text ? number(*total_text) : std::optional<std::uint64_t>{0};
  const auto done = done_text ? number(*done_text) : std::optional<std::uint64_t>{0};
  return total && done && *done >= *total;
}
using AppTypes = std::map<std::string, bool>;
std::optional<bool> playable_type(std::string type) {
  type=lower(std::move(type));
  if(type=="game"||type=="demo"||type=="mod"||type=="beta")return true;
  for(const auto* support:{"tool","application","config","dlc","music","video","hardware","guide","advertising","launcher"})
    if(type==support)return false;
  return {};
}
bool playable_id(const std::string& id,const AppTypes& types) {
  if(const auto found=types.find(id);found!=types.end())return found->second;
  // Stable provider identities, confirmed as common.type=Tool in Steam's
  // metadata: Steamworks Common Redistributables and Ubisoft Connect Services.
  // Product names/publishers are deliberately not used as a blacklist.
  return id!="228980"&&id!="4099820";
}
class AppInfo {
public:
  explicit AppInfo(std::string_view bytes):bytes_(bytes){}
  AppTypes parse() {
    std::size_t at=0;
    // v29 adds an indexed key string table. Other versions are optional,
    // unsupported metadata, not evidence that an installed title is a tool.
    if(u32(at,bytes_.size())!=0x07564429)return {};
    const auto universe=u32(at,bytes_.size());if(!universe||universe>5)return {};
    const auto offset=u64(at,bytes_.size());
    if(offset<16||offset>bytes_.size()-4)throw std::runtime_error("Invalid Steam appinfo string table");
    const auto table=static_cast<std::size_t>(offset);auto strings=table;
    const auto count=u32(strings,bytes_.size());if(count>100000)throw std::runtime_error("Excessive Steam appinfo keys");
    keys_.reserve(count);
    for(std::uint32_t i=0;i<count;++i)keys_.push_back(string(strings,bytes_.size()));
    AppTypes result;std::set<std::uint32_t> ids;bool terminated=false;
    while(at<table) {
      const auto id=u32(at,table);
      if(!id){terminated=true;break;}
      if(ids.size()>=100000||!ids.insert(id).second)throw std::runtime_error("Duplicate/excessive Steam appinfo records");
      const auto size=u32(at,table);
      if(size<60||size>table-at)throw std::runtime_error("Invalid Steam appinfo record bounds");
      const auto end=at+size;
      if(size<=4*1024*1024) {
        auto payload=at+60;std::optional<std::string> type;
        try {
          object(payload,end,0,0,type);
          if(payload!=end)throw std::runtime_error("Trailing Steam appinfo record bytes");
          if(type)if(const auto playable=playable_type(*type))result.emplace(std::to_string(id),*playable);
        } catch(const std::runtime_error&) { /* This product remains unknown; independent records still apply. */ }
      }
      at=end;
    }
    if(!terminated||at!=table)throw std::runtime_error("Missing Steam appinfo record terminator");
    return result;
  }
private:
  std::string_view bytes_;
  std::vector<std::string_view> keys_;
  std::size_t nodes_{};
  void require(std::size_t at,std::size_t count,std::size_t end) const {
    if(at>end||count>end-at||end>bytes_.size())throw std::runtime_error("Truncated Steam appinfo data");
  }
  std::uint32_t u32(std::size_t& at,std::size_t end) const {
    require(at,4,end);std::uint32_t result{};
    for(unsigned i=0;i<4;++i)result|=std::uint32_t(static_cast<unsigned char>(bytes_[at+i]))<<(i*8);
    at+=4;return result;
  }
  std::uint64_t u64(std::size_t& at,std::size_t end) const {
    const auto low=u32(at,end),high=u32(at,end);return low|(std::uint64_t(high)<<32);
  }
  std::string_view string(std::size_t& at,std::size_t end) const {
    require(at,1,end);const auto segment=bytes_.substr(at,std::min<std::size_t>(end-at,32769));
    const auto length=segment.find('\0');
    if(length==segment.npos)throw std::runtime_error("Invalid Steam appinfo string");
    const auto result=segment.substr(0,length);at+=length+1;return result;
  }
  // Only root/appinfo/common/type is captured. Other values are validated and
  // skipped without constructing a tree or retaining launch/private fields.
  void object(std::size_t& at,std::size_t end,unsigned depth,unsigned section,std::optional<std::string>& product) {
    if(depth>16)throw std::runtime_error("Excessive Steam appinfo nesting");
    std::set<std::string> object_keys;
    while(at<end) {
      const auto kind=static_cast<unsigned char>(bytes_[at++]);if(kind==8)return;
      if(++nodes_>1000000)throw std::runtime_error("Excessive Steam appinfo fields");
      const auto index=u32(at,end);if(index>=keys_.size())throw std::runtime_error("Invalid Steam appinfo key index");
      const auto key=lower(std::string(keys_[index]));
      if(object_keys.size()>=65536||!object_keys.insert(key).second)throw std::runtime_error("Duplicate/excessive Steam appinfo object keys");
      if(((section==0&&depth==0&&key=="appinfo")||(section==1&&key=="common"))&&kind!=0)
        throw std::runtime_error("Steam appinfo product container must be an object");
      if(section==2&&key=="type"&&kind!=1)throw std::runtime_error("Steam appinfo product type must be a string");
      if(kind==0) {
        const auto child=section==0&&depth==0&&key=="appinfo"?1u:section==1&&key=="common"?2u:3u;
        object(at,end,depth+1,child,product);
      } else if(kind==1) {
        const auto value=string(at,end);
        if(section==2&&key=="type") {
          if(product||!safe_text(std::string(value),64))throw std::runtime_error("Invalid/duplicate Steam product type");
          product=value;
        }
      } else if(kind==2||kind==3||kind==4||kind==6) {require(at,4,end);at+=4;}
      else if(kind==7||kind==10) {require(at,8,end);at+=8;}
      else if(kind==5) {
        std::size_t units=0;
        for(;;) {require(at,2,end);const bool zero=bytes_[at]==0&&bytes_[at+1]==0;at+=2;if(zero)break;if(++units>32768)throw std::runtime_error("Oversized Steam wide string");}
      } else throw std::runtime_error("Unsupported Steam appinfo value type");
    }
    throw std::runtime_error("Unterminated Steam appinfo object");
  }
};
std::shared_ptr<const AppTypes> app_types(const fs::path& root,Budget& budget) {
  static const auto empty=std::make_shared<const AppTypes>();
  const auto path=root/"appcache/appinfo.vdf";std::error_code error;
  if(!fs::is_regular_file(fs::symlink_status(path,error))||error)return empty;
  const auto size=fs::file_size(path,error);if(error||size<16||size>64*1024*1024||size>budget.bytes)return empty;
  budget.bytes-=size;
  const auto changed=fs::last_write_time(path,error);if(error)return empty;
  struct Cached {std::uintmax_t size{};fs::file_time_type changed{};std::shared_ptr<const AppTypes> types;};
  static std::mutex mutex;static std::map<fs::path,Cached> cache;
  std::scoped_lock lock(mutex);
  if(const auto found=cache.find(path);found!=cache.end()&&found->second.size==size&&found->second.changed==changed)return found->second.types;
  auto types=empty;
  try {
    std::ifstream input(path,std::ios::binary);std::string bytes(static_cast<std::size_t>(size)+1,'\0');
    input.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));
    if(input.bad()||static_cast<std::uint64_t>(input.gcount())!=size)throw std::runtime_error("Steam appinfo changed during its read");
    bytes.resize(static_cast<std::size_t>(size));types=std::make_shared<const AppTypes>(AppInfo(bytes).parse());
    if(fs::file_size(path,error)!=size||error||fs::last_write_time(path,error)!=changed||error)return empty;
  } catch(const std::exception&) { /* Optional malformed/unknown metadata never hides unknown games. */ }
  if(cache.size()>=64)cache.clear();
  cache[path]={size,changed,types};return types;
}
Game read_manifest(const fs::path& manifest, const fs::path& root, const std::string& id, const fs::path& executable, Budget& budget) {
  const auto bytes = read_text(manifest, 1024 * 1024, budget);
  const auto document = KeyValues(bytes, 8192).parse(); const auto* state = document.get("appstate");
  if (document.fields.size() != 1 || !state || !state->object) throw std::runtime_error("Invalid Steam app manifest root");
  const auto* stored_id = state->value("appid"); const auto* flags = state->value("stateflags"); const auto* name = state->value("name"); const auto* directory = state->value("installdir");
  if (!stored_id || app_id(*stored_id) != id || !flags || *flags != "4" || !name || !safe_text(*name, 512) || !directory || !safe_text(*directory, 32768)) throw std::runtime_error("Steam app is malformed or not fully installed");
  if (!complete_counter(*state, "bytestodownload", "bytesdownloaded") || !complete_counter(*state, "bytestostage", "bytesstaged")) throw std::runtime_error("Steam app download/staging is incomplete");
  for (const auto* key : {"buildid", "targetbuildid"}) if (state->get(key) && (!state->value(key) || !number(*state->value(key)))) throw std::runtime_error("Malformed Steam build metadata");
  if (const auto* target = state->value("targetbuildid"); target && *target != "0") {
    const auto* build = state->value("buildid"); if (!number(*target) || !build || !number(*build) || *target != *build) throw std::runtime_error("Steam target build is not installed");
  }
  auto portable = *directory; std::replace(portable.begin(), portable.end(), '\\', '/');
  const auto relative = path_from_text(portable);
  if (relative.is_absolute() || relative.has_root_name() || portable.find(':') != std::string::npos) throw std::runtime_error("Steam install directory must be relative to common");
  for (const auto& component : relative) if (component == ".." || component == ".") throw std::runtime_error("Steam install directory traverses outside common");
  const auto install = root / "steamapps/common" / relative;
  std::error_code error; if (!fs::is_directory(install, error)) throw std::runtime_error("Steam install directory is missing");
  const auto canonical = fs::weakly_canonical(install);
  if (network_path(canonical) && !network_path(root)) throw std::runtime_error("Automatic Steam discovery does not follow remote install directories outside its selected root");
  Game game; game.runner = "steam"; game.content = path_from_text(id); game.title_id = id; game.title = *name;
  game.platform = "Steam"; game.category = "Steam"; game.description = "Installed Steam application  App ID " + id;
  game.working_directory = canonical; game.executable = executable; return game;
}
bool regular_asset(const fs::path& path) { std::error_code error; return fs::is_regular_file(fs::symlink_status(path, error)); }
void artwork(Game& game, const std::vector<fs::path>& caches, Budget& budget) {
  const auto id = text(game.content);
  const auto select = [&](const fs::path& directory, const std::string& prefix) {
    if (game.icon.empty()) for (const auto* name : {"library_header.jpg", "header.jpg", "icon.jpg", "icon.png"}) if (regular_asset(directory / (prefix + name))) { game.icon = directory / (prefix + name); break; }
    if (game.background.empty()) for (const auto* name : {"library_hero.jpg", "hero.jpg"}) if (regular_asset(directory / (prefix + name))) { game.background = directory / (prefix + name); break; }
  };
  for (const auto& cache : caches) {
    select(cache, id + '_'); const auto app = cache / id; select(app, "");
    if ((game.icon.empty() || game.background.empty()) && budget.asset_entries < 20000) {
      std::vector<fs::path> directories; std::error_code error;
      for (fs::directory_iterator it(app, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
        if (++budget.asset_entries > 20000 || directories.size() >= 64) break;
        const auto name = text(it->path().filename());
        if (name.size() != 40 || !std::ranges::all_of(name, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); })) continue;
        if (!it->is_symlink(error) && it->is_directory(error)) directories.push_back(it->path());
      }
      std::ranges::sort(directories);
      for (const auto& directory : directories) { select(directory, ""); if (!game.icon.empty() && !game.background.empty()) break; }
    }
    if (!game.icon.empty() && !game.background.empty()) break;
  }
  if (game.background.empty() && text(game.icon.filename()).find("header") != std::string::npos) game.background = game.icon;
}
}
bool steam_game_is_playable(const Game& game) {
  if(game.runner!="steam")return true;
  const auto id=app_id(text(game.content));if(!id)return true;
  Budget budget;std::vector<fs::path> roots;
  // A selected/explicit library comes first; its own cache can classify a
  // title without depending on another Steam installation or test fixture.
  auto path=game.working_directory;
  for(unsigned depth=0;depth<16&&!path.empty();++depth) {
    if(path.filename()=="common"&&path.parent_path().filename()=="steamapps") {roots.push_back(path.parent_path().parent_path());break;}
    const auto parent=path.parent_path();if(parent==path)break;path=parent;
  }
  if(game.executable.has_parent_path())roots.push_back(game.executable.parent_path());
  const auto defaults=default_roots();roots.insert(roots.end(),defaults.begin(),defaults.end());
  std::set<fs::path> seen;
  for(const auto& root:roots) {
    if(network_path(root)||!seen.insert(root.lexically_normal()).second)continue;
    const auto types=app_types(root,budget);
    if(const auto found=types->find(*id);found!=types->end())return found->second;
  }
  return playable_id(*id,{});
}
GameDiscovery discover_steam_games(const std::vector<fs::path>& installation_roots, const fs::path& steam_executable) {
  if (installation_roots.size() > 64) throw ScanLimit("Choose at most 64 Steam installation roots");
  if (!safe_text(text(steam_executable), 32768, false)) throw std::runtime_error("Invalid Steam executable path");
  Budget budget; GameDiscovery games; std::vector<fs::path> roots; std::set<std::string> seen_roots, seen_apps;
  const auto append = [&](const fs::path& path, bool explicit_root) {
    if (!safe_text(text(path), 32768) || (!explicit_root && network_path(path))) return;
    std::error_code error; if (!fs::is_directory(path, error)) return;
    const auto canonical = fs::weakly_canonical(fs::absolute(path));
    if (!explicit_root && network_path(canonical)) return;
    const auto key =
#ifdef _WIN32
      lower(text(canonical));
#else
      text(canonical);
#endif
    if (seen_roots.contains(key)) return;
    if (roots.size() >= 64) throw ScanLimit("Steam discovery exceeded 64 library roots");
    seen_roots.insert(key); roots.push_back(canonical);
  };
  auto seeds = installation_roots.empty() ? default_roots() : installation_roots;
  if (installation_roots.empty() && steam_executable.has_parent_path()) seeds.push_back(steam_executable.parent_path());
  for (const auto& root : seeds) append(root, !installation_roots.empty());
  const auto initial = roots;
  for (const auto& root : initial) {
    try { for (const auto& registered : registered_paths(root, budget)) append(registered, false); }
    catch (const ScanLimit&) { throw; } catch (const std::exception&) { /* Invalid registry cannot hide this root's valid manifests. */ }
  }
  std::vector<fs::path> caches;
  AppTypes product_types;
  for(const auto& root:roots) {const auto types=app_types(root,budget);product_types.insert(types->begin(),types->end());}
  for (const auto& root : roots) { std::error_code error; if (fs::is_directory(root / "appcache/librarycache", error)) caches.push_back(root / "appcache/librarycache"); }
  for (const auto& root : roots) {
    std::vector<std::pair<fs::path, std::string>> manifests; std::error_code error;
    for (fs::directory_iterator it(root / "steamapps", fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
      if (++budget.entries > 50000) throw ScanLimit("Steam discovery exceeded 50000 directory entries");
      const auto name = lower(text(it->path().filename()));
      if (!name.starts_with("appmanifest_") || !name.ends_with(".acf")) continue;
      const auto id = app_id(name.substr(12, name.size() - 16));
      if (!id || it->is_symlink(error) || !it->is_regular_file(error)) continue;
      if (++budget.manifests > 20000) throw ScanLimit("Steam discovery exceeded 20000 app manifests");
      manifests.emplace_back(it->path(), *id);
    }
    std::ranges::sort(manifests);
    for (const auto& [manifest, id] : manifests) {
      if (seen_apps.contains(id)) continue;
      try {
        auto game = read_manifest(manifest, root, id, steam_executable, budget);
        if(!playable_id(id,product_types)) {seen_apps.insert(id);continue;}
        artwork(game, caches, budget); seen_apps.insert(id); games.push_back({std::move(game), true});
      } catch (const ScanLimit&) { throw; } catch (const std::exception&) { /* Read-only snapshot: incomplete/corrupt apps are skipped. */ }
      if (games.size() > 10000) throw ScanLimit("Steam discovery exceeded 10000 installed applications");
    }
  }
  return games;
}
} // namespace openxmb::desktop
