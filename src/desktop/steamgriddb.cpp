#include "openxmb/desktop/steamgriddb.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace openxmb::desktop {
namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;
constexpr std::size_t query_limit = 2u * 1024u * 1024u;
constexpr std::uint64_t ttl = 24 * 60 * 60;
std::atomic<std::uint64_t> sequence{};
const std::string api = "https://www.steamgriddb.com/api/v2";
std::uint64_t wall_time() { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }
const char* route(SteamGridDBRole role) {
  switch (role) { case SteamGridDBRole::icon: return "icons"; case SteamGridDBRole::horizontal_grid: return "grids"; case SteamGridDBRole::logo: return "logos"; case SteamGridDBRole::hero: return "heroes"; }
  throw std::runtime_error("Unsupported artwork role");
}
const char* cdn_route(SteamGridDBRole role) {
  switch (role) { case SteamGridDBRole::icon: return "icon"; case SteamGridDBRole::horizontal_grid: return "grid"; case SteamGridDBRole::logo: return "logo"; case SteamGridDBRole::hero: return "hero"; }
  throw std::runtime_error("Unsupported artwork role");
}
std::size_t image_limit(SteamGridDBRole role) { return role == SteamGridDBRole::hero ? 32u * 1024u * 1024u : 16u * 1024u * 1024u; }
std::string fingerprint(std::string_view value) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const unsigned char byte : value) { hash ^= byte; hash *= 1099511628211ull; }
  std::ostringstream out; out << std::hex << std::setw(16) << std::setfill('0') << hash; return out.str();
}
bool safe_string(const std::string& value, std::size_t maximum, bool nonempty = true) {
  if ((nonempty && value.empty()) || value.size() > maximum || std::ranges::any_of(value, [](unsigned char c) { return c < 0x20 || c == 0x7f; })) return false;
  try { (void)json(value).dump(); return true; } catch (...) { return false; }
}
json parse_json(const std::string& bytes) {
  return json::parse(bytes, [](int depth, json::parse_event_t, json&) {
    if (depth > 32) throw std::runtime_error("Provider JSON nesting exceeds its limit");
    return true;
  });
}
std::string normalized(std::string_view value) {
  std::string out; bool space = false;
  for (auto c : value) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { space = !out.empty(); continue; }
    if (space) { out += ' '; space = false; }
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    out += c;
  }
  return out;
}
std::string encode(std::string_view value) {
  constexpr char hex[] = "0123456789ABCDEF"; std::string out;
  for (const unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}
std::uint64_t positive(const json& value) {
  if (!value.is_number_integer() || value <= 0) throw std::runtime_error("Invalid provider numeric metadata");
  return value.get<std::uint64_t>();
}
bool dimensions(unsigned width, unsigned height, SteamGridDBRole role) {
  if (!width || !height || width > 8192 || height > 8192 || static_cast<std::uint64_t>(width) * height > 32u * 1024u * 1024u) return false;
  switch (role) {
  case SteamGridDBRole::horizontal_grid: return (width == 460 && height == 215) || (width == 920 && height == 430);
  case SteamGridDBRole::hero: return (width == 1920 && height == 620) || (width == 3840 && height == 1240) || (width == 1600 && height == 650);
  case SteamGridDBRole::icon: return width == height;
  case SteamGridDBRole::logo: return true;
  }
  return false;
}
bool approved_url(const std::string& url, SteamGridDBRole role) {
  if (!safe_string(url, 2048)) return false;
  const std::string host = "https://cdn2.steamgriddb.com/";
  if (!url.starts_with(host)) return false;
  auto path = std::string_view(url).substr(host.size());
  const auto prefix = std::string(cdn_route(role)) + '/';
  if (path.starts_with("file/sgdb-cdn/")) path.remove_prefix(14);
  if (!path.starts_with(prefix)) return false;
  path.remove_prefix(prefix.size());
  if (path.empty() || path.find("..") != std::string_view::npos || !std::ranges::all_of(path, [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'; })) return false;
  return path.ends_with(".png") || path.ends_with(".jpg") || path.ends_with(".jpeg") || path.ends_with(".webp");
}
std::string mime_name(std::string mime) { return mime == "image/jpg" ? "image/jpeg" : mime; }
const char* extension(const std::string& mime) { return mime == "image/png" ? ".png" : mime == "image/jpeg" ? ".jpg" : mime == "image/webp" ? ".webp" : ""; }
json clean_search(const json& data) {
  if (!data.is_array() || data.size() > 128) throw std::runtime_error("Invalid search response");
  json result = json::array(); std::set<std::uint64_t> seen;
  for (const auto& item : data) {
    const auto id = positive(item.at("id")); const auto name = item.at("name").get<std::string>();
    if (!safe_string(name, 512)) throw std::runtime_error("Invalid search title");
    if (seen.insert(id).second) result.push_back({{"id", id}, {"name", name}});
  }
  return result;
}
json clean_assets(const json& data, SteamGridDBRole role) {
  if (!data.is_array() || data.size() > 50) throw std::runtime_error("Invalid asset response");
  json result = json::array(); std::set<std::uint64_t> seen;
  for (const auto& item : data) {
    const auto id = positive(item.at("id")); const auto width = positive(item.at("width")), height = positive(item.at("height"));
    const auto mime = mime_name(item.at("mime").get<std::string>()); const auto url = item.at("url").get<std::string>();
    if (width > 8192 || height > 8192 || !dimensions(static_cast<unsigned>(width), static_cast<unsigned>(height), role) || !*extension(mime) || !approved_url(url, role)) continue;
    if (item.contains("animated") && (!item.at("animated").is_boolean() || item.at("animated").get<bool>())) continue;
    if (seen.insert(id).second) result.push_back({{"id", id}, {"width", width}, {"height", height}, {"mime", mime}, {"url", url}});
  }
  return result;
}
std::string read_file(const fs::path& path, std::size_t maximum) {
  std::error_code error;
  if (!fs::is_regular_file(fs::symlink_status(path, error))) throw std::runtime_error("Cache file missing");
  const auto size = fs::file_size(path); if (size > maximum) throw std::runtime_error("Cache file exceeds limit");
  std::ifstream input(path, std::ios::binary); std::string bytes(static_cast<std::size_t>(size) + 1, '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (input.bad() || static_cast<std::uint64_t>(input.gcount()) != size) throw std::runtime_error("Cache read failed");
  bytes.resize(static_cast<std::size_t>(size)); return bytes;
}
void atomic_write(const fs::path& root, const fs::path& path, const std::string& bytes) {
  fs::create_directories(root);
  std::uint64_t total = 0; std::size_t entries = 0;
  for (const auto& entry : fs::directory_iterator(root)) {
    if (++entries > 2048) throw std::runtime_error("Cache is full");
    if (entry.path() != path && fs::is_regular_file(entry.symlink_status())) total += entry.file_size();
    if (total > 512u * 1024u * 1024u) throw std::runtime_error("Cache is full");
  }
  if (bytes.size() > 512u * 1024u * 1024u - total) throw std::runtime_error("Cache is full");
  const auto process_id =
#ifdef _WIN32
    GetCurrentProcessId();
#else
    getpid();
#endif
  auto temporary = path; temporary += ".tmp-" + std::to_string(process_id) + '-' + std::to_string(wall_time()) + '-' + std::to_string(++sequence);
  try {
    { std::ofstream output(temporary, std::ios::binary | std::ios::trunc); output.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); output.flush(); if (!output) throw std::runtime_error("Cache save failed"); }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cache replace failed");
#else
    const int handle = open(temporary.c_str(), O_RDONLY | O_CLOEXEC);
    if (handle < 0) throw std::runtime_error("Cache flush failed");
    const auto flushed = fsync(handle); close(handle); if (flushed != 0) throw std::runtime_error("Cache flush failed");
    fs::rename(temporary, path);
#endif
  } catch (...) { std::error_code ignored; fs::remove(temporary, ignored); throw; }
}
unsigned byte(std::string_view bytes, std::size_t at) { if (at >= bytes.size()) throw std::runtime_error("Truncated image"); return static_cast<unsigned char>(bytes[at]); }
std::uint32_t big32(std::string_view bytes, std::size_t at) { return (byte(bytes, at) << 24) | (byte(bytes, at + 1) << 16) | (byte(bytes, at + 2) << 8) | byte(bytes, at + 3); }
std::uint32_t little32(std::string_view bytes, std::size_t at) { return byte(bytes, at) | (byte(bytes, at + 1) << 8) | (byte(bytes, at + 2) << 16) | (byte(bytes, at + 3) << 24); }
std::uint32_t crc32(std::string_view bytes) {
  std::uint32_t crc = 0xffffffffu;
  for (const unsigned char value : bytes) {
    crc ^= value;
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
  }
  return crc ^ 0xffffffffu;
}
struct Image { unsigned width{}, height{}; std::string mime; };
Image image_header(std::string_view bytes) {
  if (bytes.starts_with(std::string_view("\x89PNG\r\n\x1a\n", 8))) {
    Image image; image.mime = "image/png"; bool header = false, data = false; std::size_t at = 8;
    for (unsigned chunks = 0; chunks < 4096 && at < bytes.size(); ++chunks) {
      if (bytes.size() - at < 12) throw std::runtime_error("Truncated PNG");
      const auto length = big32(bytes, at); const auto type = bytes.substr(at + 4, 4);
      if (length > bytes.size() - at - 12) throw std::runtime_error("Truncated PNG chunk");
      if (!header && type != "IHDR") throw std::runtime_error("PNG is missing IHDR");
      if (type == "IHDR") {
        if (header || length != 13) throw std::runtime_error("Invalid PNG header");
        if (crc32(bytes.substr(at + 4, 17)) != big32(bytes, at + 21)) throw std::runtime_error("Invalid PNG header checksum");
        image.width = big32(bytes, at + 8); image.height = big32(bytes, at + 12); header = true;
        const auto depth = byte(bytes, at + 16), color = byte(bytes, at + 17);
        if ((depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) || (color != 0 && color != 2 && color != 3 && color != 4 && color != 6) || (color == 3 && depth == 16) || ((color == 2 || color == 4 || color == 6) && depth < 8)) throw std::runtime_error("Invalid PNG pixel format");
        if (byte(bytes, at + 18) || byte(bytes, at + 19) || byte(bytes, at + 20) > 1) throw std::runtime_error("Invalid PNG encoding");
      } else if (type == "acTL") throw std::runtime_error("Animated artwork is unsupported");
      else if (type == "IDAT") data = true;
      else if (type == "IEND") { if (length || !data || at + 12 != bytes.size()) throw std::runtime_error("Invalid PNG end"); return image; }
      at += static_cast<std::size_t>(length) + 12;
    }
    throw std::runtime_error("PNG has no valid end");
  }
  if (bytes.size() >= 4 && byte(bytes, 0) == 0xff && byte(bytes, 1) == 0xd8) {
    Image image; image.mime = "image/jpeg"; std::size_t at = 2;
    for (unsigned segments = 0; segments < 1024 && at < bytes.size(); ++segments) {
      if (byte(bytes, at++) != 0xff) throw std::runtime_error("Invalid JPEG marker");
      while (byte(bytes, at) == 0xff) ++at;
      const auto marker = byte(bytes, at++);
      if (marker == 0xd9) break;
      if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
      const auto length = (byte(bytes, at) << 8) | byte(bytes, at + 1);
      if (length < 2 || length > bytes.size() - at) throw std::runtime_error("Truncated JPEG segment");
      if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
        if (length < 8) throw std::runtime_error("Invalid JPEG frame");
        image.height = (byte(bytes, at + 3) << 8) | byte(bytes, at + 4); image.width = (byte(bytes, at + 5) << 8) | byte(bytes, at + 6);
      }
      if (marker == 0xda) {
        if (!image.width || !image.height || byte(bytes, bytes.size() - 2) != 0xff || byte(bytes, bytes.size() - 1) != 0xd9) throw std::runtime_error("Invalid JPEG data/end");
        return image;
      }
      at += length;
    }
    throw std::runtime_error("JPEG has no complete frame");
  }
  if (bytes.size() >= 20 && bytes.substr(0, 4) == "RIFF" && bytes.substr(8, 4) == "WEBP" && static_cast<std::uint64_t>(little32(bytes, 4)) + 8 == bytes.size()) {
    Image image; image.mime = "image/webp"; bool codec = false; std::size_t at = 12;
    for (unsigned chunks = 0; chunks < 1024 && at < bytes.size(); ++chunks) {
      if (bytes.size() - at < 8) throw std::runtime_error("Truncated WebP");
      const auto type = bytes.substr(at, 4); const auto length = little32(bytes, at + 4); const auto begin = at + 8;
      if (length > bytes.size() - begin) throw std::runtime_error("Truncated WebP chunk");
      unsigned width = 0, height = 0;
      if (type == "ANIM" || type == "ANMF") throw std::runtime_error("Animated artwork is unsupported");
      if (type == "VP8X") {
        if (length != 10 || (byte(bytes, begin) & 2)) throw std::runtime_error("Invalid WebP extended header");
        width = 1 + byte(bytes, begin + 4) + (byte(bytes, begin + 5) << 8) + (byte(bytes, begin + 6) << 16);
        height = 1 + byte(bytes, begin + 7) + (byte(bytes, begin + 8) << 8) + (byte(bytes, begin + 9) << 16);
      } else if (type == "VP8 ") {
        if (length < 10 || bytes.substr(begin + 3, 3) != std::string_view("\x9d\x01\x2a", 3)) throw std::runtime_error("Invalid WebP frame");
        width = (byte(bytes, begin + 6) | (byte(bytes, begin + 7) << 8)) & 0x3fff; height = (byte(bytes, begin + 8) | (byte(bytes, begin + 9) << 8)) & 0x3fff; codec = true;
      } else if (type == "VP8L") {
        if (length < 5 || byte(bytes, begin) != 0x2f) throw std::runtime_error("Invalid WebP lossless frame");
        const auto packed = little32(bytes, begin + 1); if (packed >> 29) throw std::runtime_error("Invalid WebP version");
        width = 1 + (packed & 0x3fff); height = 1 + ((packed >> 14) & 0x3fff); codec = true;
      }
      if (width && height) { if (image.width && (image.width != width || image.height != height)) throw std::runtime_error("Conflicting WebP dimensions"); image.width = width; image.height = height; }
      at = begin + length + (length & 1u); if (at > bytes.size()) throw std::runtime_error("Truncated WebP padding");
    }
    if (codec && at == bytes.size()) return image;
  }
  throw std::runtime_error("Unsupported or malformed image");
}
void validate_image(const std::string& bytes, const SteamGridDBAsset& asset) {
  const auto image = image_header(bytes);
  if (image.mime != asset.mime || image.width != asset.width || image.height != asset.height || !dimensions(image.width, image.height, asset.role)) throw std::runtime_error("Image dimensions/type do not match selected artwork");
}
fs::path cache_root(fs::path root) {
  if (root.empty() || root.native().size() > 32768 || root.native().find(fs::path::value_type{}) != fs::path::string_type::npos) throw std::invalid_argument("Provide a valid profile artwork cache directory");
  return fs::absolute(std::move(root)).lexically_normal();
}
}
struct SteamGridDB::Impl {
  fs::path root;
  HttpTransport transport;
  std::function<std::uint64_t()> clock;
  std::mutex mutex;
  std::map<std::string, std::uint64_t> retry_after;
  struct Query { bool success{}, cached{}, stale{}; json data; std::string message; };
  Impl(fs::path value, HttpTransport client, std::function<std::uint64_t()> now) : root(cache_root(std::move(value))), transport(std::move(client)), clock(std::move(now)) {
    if (!transport) transport = http_get;
    if (!clock) clock = wall_time;
  }
  Query query(const std::string& path, const std::string& key, const std::string& credential, const std::function<json(const json&)>& clean) {
    Query cached; const auto now = clock(); const auto destination = root / ("q-" + fingerprint(key) + ".json");
    try {
      const auto value = parse_json(read_file(destination, query_limit));
      const auto stamp = positive(value.at("fetched_at"));
      if (value.value("version", 0) != 1 || value.at("key") != key || stamp > now + 300) throw std::runtime_error("Invalid query cache");
      cached = {true, true, now < stamp || now - stamp >= ttl, clean(value.at("data")), "Using cached SteamGridDB results"};
      if (!cached.stale) return cached;
    } catch (...) { cached = {}; }
    const auto fail = [&](std::string message) { if (cached.success) { cached.message = "Using stale cached SteamGridDB results; " + message; return cached; } return Query{false, false, false, {}, std::move(message)}; };
    if (credential.empty()) return fail("Set a SteamGridDB API key for an online lookup");
    if (!safe_string(credential, 512) || credential.find(' ') != std::string::npos) return fail("The SteamGridDB API key is invalid");
    if (const auto found = retry_after.find(key); found != retry_after.end() && now < found->second && found->second - now <= 60) return fail("SteamGridDB rate limited this lookup (HTTP 429); retry later");
    HttpRequest request;
    request.url = api + path; request.headers = {"Authorization: Bearer " + credential, "Accept: application/json"};
    request.max_bytes = query_limit; request.timeout_seconds = 20; request.max_redirects = 0; request.sensitive = true;
    HttpResponse response;
    try { response = transport(request); } catch (...) { return fail("SteamGridDB request failed"); }
    if (response.status == 429) { if (retry_after.size() >= 128) retry_after.erase(retry_after.begin()); retry_after[key] = now + 60; return fail("SteamGridDB rate limited this lookup (HTTP 429); retry later"); }
    if (!response.success || response.status != 200) return fail(response.status == 401 || response.status == 403 ? "SteamGridDB rejected the API key" : "SteamGridDB request failed");
    if (response.body.size() > query_limit) return fail("SteamGridDB metadata exceeded its size limit");
    try {
      const auto body = parse_json(response.body);
      if (!body.is_object() || !body.value("success", false)) return fail("SteamGridDB returned an unsuccessful response");
      auto data = clean(body.at("data"));
      Query result{true, false, false, std::move(data), "SteamGridDB results ready; choose the game and asset explicitly"};
      const auto encoded = json{{"version", 1}, {"key", key}, {"fetched_at", now}, {"data", result.data}}.dump();
      if (encoded.find(credential) == std::string::npos) {
        try { atomic_write(root, destination, encoded); } catch (...) { result.message += "; local cache could not be saved"; }
      }
      retry_after.erase(key); return result;
    } catch (...) { return fail("SteamGridDB returned malformed metadata"); }
  }
};
SteamGridDB::SteamGridDB(fs::path root, HttpTransport transport, std::function<std::uint64_t()> clock) : impl_(std::make_unique<Impl>(std::move(root), std::move(transport), std::move(clock))) {}
SteamGridDB::~SteamGridDB() = default;
SteamGridDBSearchResult SteamGridDB::search(const std::string& title, const std::string& key) {
  SteamGridDBSearchResult result; const auto term = normalized(title);
  if (!safe_string(term, 512)) { result.message = "Enter a valid game title of at most 512 bytes"; return result; }
  std::lock_guard lock(impl_->mutex); const auto query = impl_->query("/search/autocomplete/" + encode(term), "search/" + term, key, clean_search);
  result.success = query.success; result.from_cache = query.cached; result.stale = query.stale; result.message = query.message;
  if (query.success) for (const auto& game : query.data) { const auto name = game.at("name").get<std::string>(); result.games.push_back({positive(game.at("id")), name, normalized(name) == term}); }
  return result;
}
SteamGridDBAssetsResult SteamGridDB::assets(std::uint64_t game_id, SteamGridDBRole role, const std::string& key, unsigned page) {
  SteamGridDBAssetsResult result; result.page = page;
  try {
    if (!game_id || page > 1000) throw std::runtime_error("Choose a valid provider game ID and page between 0 and 1000");
    const auto family = std::string(route(role));
    auto path = '/' + family + "/game/" + std::to_string(game_id) + "?limit=50&page=" + std::to_string(page);
    if (role != SteamGridDBRole::logo) path += "&types=static";
    if (role == SteamGridDBRole::horizontal_grid) path += "&dimensions=460x215,920x430";
    std::lock_guard lock(impl_->mutex);
    const auto query = impl_->query(path, family + '/' + std::to_string(game_id) + '/' + std::to_string(page), key, [role](const json& data) { return clean_assets(data, role); });
    result.success = query.success; result.from_cache = query.cached; result.stale = query.stale; result.message = query.message;
    if (query.success) for (const auto& asset : query.data) result.assets.push_back({positive(asset.at("id")), game_id, role, asset.at("url"), asset.at("mime"), asset.at("width"), asset.at("height")});
    result.has_more = query.success && query.data.size() == 50;
  } catch (...) { result.success = false; result.message = "Choose a supported artwork role, provider game ID and page between 0 and 1000"; }
  return result;
}
SteamGridDBDownloadResult SteamGridDB::select(std::uint64_t game_id, std::uint64_t asset_id, SteamGridDBRole role, const fs::path& native, SteamGridDBOverride policy, const std::string& key, unsigned page) {
  SteamGridDBDownloadResult result;
  if (policy != SteamGridDBOverride::preserve_native && policy != SteamGridDBOverride::override_native) { result.message = "Choose an explicit native-artwork override policy"; return result; }
  if (policy == SteamGridDBOverride::preserve_native && !native.empty()) {
    std::error_code error;
    if (native.native().find(fs::path::value_type{}) != fs::path::string_type::npos) { result.message = "The native artwork path is invalid"; return result; }
    if (fs::is_regular_file(native, error)) { result.success = true; result.used_native = true; result.path = native; result.message = "Preserving native artwork; provider replacement requires explicit override"; return result; }
  }
  if (!asset_id) { result.message = "Choose an explicit provider asset ID"; return result; }
  const auto choices = assets(game_id, role, key, page);
  if (!choices) { result.message = choices.message; return result; }
  const auto found = std::ranges::find_if(choices.assets, [&](const auto& asset) { return asset.id == asset_id; });
  if (found == choices.assets.end()) { result.message = "The selected asset is not a supported match in this game/role/page; choose an asset or another page"; return result; }
  const auto asset = *found; std::lock_guard lock(impl_->mutex);
  const auto download_key = "download/" + std::string(route(role)) + '/' + std::to_string(asset.id) + '/' + fingerprint(asset.url);
  const auto path = impl_->root / ("image-" + std::string(route(role)) + '-' + std::to_string(asset.id) + '-' + fingerprint(asset.url) + extension(asset.mime));
  try {
    const auto bytes = read_file(path, image_limit(role)); validate_image(bytes, asset);
    result = {true, true, false, path, asset.width, asset.height, "Using cached artwork with original dimensions"}; return result;
  } catch (...) { /* An incomplete/corrupt cache file is never handed to the UI. */ }
  const auto now = impl_->clock();
  if (const auto retry = impl_->retry_after.find(download_key); retry != impl_->retry_after.end() && now < retry->second && retry->second - now <= 60) { result.message = "SteamGridDB artwork download was rate limited (HTTP 429); retry later"; return result; }
  HttpResponse response;
  HttpRequest request; request.url = asset.url; request.max_bytes = image_limit(role); request.timeout_seconds = 20; request.max_redirects = 0;
  try { response = impl_->transport(request); }
  catch (...) { result.message = "SteamGridDB artwork download failed"; return result; }
  if (!response.success || response.status != 200) {
    if (response.status == 429) { if (impl_->retry_after.size() >= 128) impl_->retry_after.erase(impl_->retry_after.begin()); impl_->retry_after[download_key] = now + 60; }
    result.message = response.status == 429 ? "SteamGridDB artwork download was rate limited (HTTP 429)" : "SteamGridDB artwork download failed"; return result;
  }
  if (response.body.size() > image_limit(role)) { result.message = "SteamGridDB artwork exceeded its download size limit"; return result; }
  try { validate_image(response.body, asset); }
  catch (...) { result.message = "SteamGridDB artwork bytes, type or dimensions do not match the selected asset"; return result; }
  if (!key.empty() && response.body.find(key) != std::string::npos) { result.message = "SteamGridDB artwork response was rejected"; return result; }
  try { atomic_write(impl_->root, path, response.body); }
  catch (...) { result.message = "SteamGridDB artwork cache is full or unwritable"; return result; }
  impl_->retry_after.erase(download_key);
  return {true, false, false, path, asset.width, asset.height, "Artwork downloaded at original dimensions"};
}
} // namespace openxmb::desktop
