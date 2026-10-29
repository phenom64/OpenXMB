#include "openxmb/desktop/steamgriddb.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#ifdef OPENXMB_SGDB_STANDALONE_TESTS
namespace openxmb::desktop {
HttpResponse http_get(const HttpRequest&) { throw std::runtime_error("Tests must use their injected fake transport"); }
}
#endif
namespace fs = std::filesystem;
using namespace openxmb::desktop;
using json = nlohmann::json;
namespace {
const std::string secret = "sgdb-transient-test-token-never-persist";
void require(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }
std::string png() {
  const std::string hex =
    "89504e470d0a1a0a0000000d49484452000001cc000000d7080600000097bd616a000002ac4944415478daedd5a101000008c3b09dcee7c3a0b920220fd4349d1400f8450400304c00304c00304c00304c00304c00304c00304c00c03001c03001c03001c03001c03001c03001c0300100c30400c30400c30400c30400c30400c30400c30400c304000c13000c13000c13000c13000c13000c13000c1300304c00304c00304c00304c00304c00304c00304c00c03001c03001c03001c03001c03001c03001c03001c0300100c30400c30400c30400c30400c30400c30400c304000c13000c13000c13000c13000c13000c13000c1300304c00304c00304c00304c00304c00304c00304c00304c00c03001c03001c03001c03001c03001c03001c0300100c30400c30400c30400c30400c30400c30400c30400c31402000c13000c13000c13000c13000c13000c13000c1300304c00304c00304c00304c00304c00304c00304c00c03001c03001c03001c03001c03001c03001c03001c0300100c30400c30400c30400c30400c30400c30400c304000c13000c13000c13000c13000c13000c13000c1300304c00304c00304c00304c00304c00304c00304c00304c00c03001c03001c03001c03001c03001c03001c0300100c30400c30400c30400c30400c30400c30400c30400c31401000c13000c13000c13000c13000c13000c13000c1300304c00304c00304c00304c00304c00304c00304c00c03001c03001c03001c03001c03001c03001c03001c0300100c30400c30400c30400c30400c30400c30400c304000c13000c13000c13000c13000c13000c13000c1300304c00304c00304c00304c00304c00304c00304c00304c00c03001c03001c03001c03001c03001c03001c0300100c30400c30400c30400c30400c30400c30400c30400c31401000c13000c13000c13000c13000c13000c13000c1300304c00304c00304c00304c00304c00304c00304c00e02c740d05c633fda48f0000000049454e44ae426082";
  std::string bytes;
  for (std::size_t i = 0; i < hex.size(); i += 2) bytes += static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16));
  return bytes;
}

std::string jpeg() {
  const std::string hex = "ffd8fffe00104c61766336312e31392e31303100ffdb0043000804040404040505050505050606060606060606060606060607070708080807070706060707080808080909090808080809090a0a0a0c0c0b0b0e0e0e111114ffc4004c0001010000000000000000000000000000000501010100000000000000000000000000000007100100000000000000000000000000000000110100000000000000000000000000000000ffc000110800d701cc03011200021200031200ffda000c03010002110311003f00ae2542940000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000003ffd9";
  std::string bytes;
  for (std::size_t i = 0; i < hex.size(); i += 2) bytes += static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16));
  return bytes;
}

std::string webp() {
  const std::string hex = "52494646220000005745425056503820160000003001009d012a010001000ec0fe25a400037000000000";
  std::string bytes;
  for (std::size_t i = 0; i < hex.size(); i += 2) bytes += static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16));
  return bytes;
}
json asset(std::uint64_t id, unsigned width = 460, unsigned height = 215, std::string url = "https://cdn2.steamgriddb.com/grid/fixture.png") {
  return {{"id", id}, {"width", width}, {"height", height}, {"mime", "image/png"}, {"url", std::move(url)}, {"score", 10}, {"style", "alternate"}, {"author", {{"name", "Fixture author"}}}};
}
HttpResponse success(json data) { return {true, 200, json{{"success", true}, {"data", std::move(data)}}.dump(), {}}; }
std::string read(const fs::path& file) { std::ifstream input(file, std::ios::binary); return {std::istreambuf_iterator<char>(input), {}}; }
void no_secret(const fs::path& root) {
  if (!fs::exists(root)) return;
  for (const auto& item : fs::recursive_directory_iterator(root)) if (item.is_regular_file()) require(read(item.path()).find(secret) == std::string::npos, "provider credentials leaked into cache files");
}
void provider(const fs::path& root) {
  std::uint64_t now = 1000000; std::vector<HttpRequest> requests;
  HttpTransport transport = [&](const HttpRequest& request) -> HttpResponse {
    requests.push_back(request);
    if (request.url.find("/search/autocomplete/") != std::string::npos) return success(json::array({{{"id", 1}, {"name", "Fixture Game"}, {"unused_secret", secret}}, {{"id", 2}, {"name", "Fixture Game Deluxe"}}, {{"id", 3}, {"name", "fixture game"}}}));
    if (request.url.find("/grids/game/42") != std::string::npos) return success(json::array({asset(101), asset(102, 600, 900), asset(103, 512, 512)}));
    if (request.url.starts_with("https://cdn2.steamgriddb.com/")) return {true, 200, png(), {}};
    return {false, 404, secret, secret};
  };
  SteamGridDB client(root / "cache", transport, [&] { return now; });
  const auto missing = client.search("Fixture Game");
  require(!missing && missing.message.find("API key") != std::string::npos && requests.empty(), "missing credentials should be actionable without making requests");
  const auto search = client.search("Fixture Game", secret);
  require(bool(search) && search.games.size() == 3 && std::ranges::count_if(search.games, [](const auto& game) { return game.exact_match; }) == 2, "search must expose exact and fuzzy candidates without choosing one silently");
  require(requests.size() == 1 && requests.back().url.starts_with("https://www.steamgriddb.com/api/v2/search/autocomplete/") && requests.back().url.find(secret) == std::string::npos && requests.back().sensitive && requests.back().max_redirects == 0, "API host/authentication request boundary is incorrect");
  require(std::ranges::find(requests.back().headers, "Authorization: Bearer " + secret) != requests.back().headers.end(), "API request is missing the bearer header");
  require(client.search("fixture game").from_cache && requests.size() == 1, "fresh normalized query cache should be reusable without credentials");
  const auto assets = client.assets(42, SteamGridDBRole::horizontal_grid, secret);
  require(bool(assets) && assets.assets.size() == 1 && assets.assets.front().id == 101 && assets.assets.front().role == SteamGridDBRole::horizontal_grid, "portrait and square grids must not be offered for horizontal artwork");
  fs::create_directories(root); const auto native = root / "ICON0.PNG"; { std::ofstream output(native); output << "native asset bytes"; }
  const auto before_native = requests.size();
  const auto preserved = client.select(9999, 8888, SteamGridDBRole::horizontal_grid, native);
  require(bool(preserved) && preserved.used_native && preserved.path == native && requests.size() == before_native, "native PS3 media must have precedence before provider I/O unless explicitly overridden");
  const auto selected = client.select(42, 101, SteamGridDBRole::horizontal_grid, native, SteamGridDBOverride::override_native, secret);
  require(bool(selected) && !selected.used_native && selected.path != native && selected.width == 460 && selected.height == 215 && read(selected.path) == png() && read(native) == "native asset bytes", "explicit override must return a separate validated provider file without modifying native assets or aspect");
  require(requests.back().headers.empty() && !requests.back().sensitive && requests.back().max_redirects == 0 && requests.back().max_bytes == 16u * 1024u * 1024u, "public CDN download must not receive a bearer token or redirect permission");
  const auto count = requests.size();
  const auto cached = client.select(42, 101, SteamGridDBRole::horizontal_grid, {}, SteamGridDBOverride::override_native);
  require(bool(cached) && cached.from_cache && requests.size() == count, "selected cached image should be available offline without redundant requests");
  require(!client.select(42, 102, SteamGridDBRole::horizontal_grid, {}, SteamGridDBOverride::override_native) && requests.size() == count, "manual asset id must not bypass role/aspect validation");
  no_secret(root);
}
void failures(const fs::path& root) {
  std::uint64_t now = 2000000; unsigned calls = 0;
  SteamGridDB limited(root / "limited", [&](const HttpRequest&) { ++calls; return HttpResponse{false, 429, secret, "Bearer " + secret}; }, [&] { return now; });
  const auto error = limited.search("Rate limited", secret);
  require(!error && error.message.find("429") != std::string::npos && error.message.find(secret) == std::string::npos, "rate limiting must be actionable and redact server/transport credential echoes");
  (void)limited.search("Rate limited", secret); require(calls == 1, "429 should not trigger redundant immediate requests");
  now += 61; (void)limited.search("Rate limited", secret); require(calls == 2, "rate-limit backoff did not permit a later retry");
  SteamGridDB malformed(root / "malformed", [](const HttpRequest&) { return HttpResponse{true, 200, "{\"success\":true,\"data\":\"wrong shape\"}", secret}; });
  require(!malformed.search("Malformed", secret), "malformed API response was accepted");
  unsigned cdn_calls = 0;
  SteamGridDB unsafe(root / "unsafe", [&](const HttpRequest& request) {
    if (request.url.find("/api/v2/") != std::string::npos) return success(json::array({asset(1, 460, 215, "https://cdn2.steamgriddb.com.evil.invalid/grid/fixture.png")}));
    ++cdn_calls; return HttpResponse{true, 200, png(), {}};
  });
  require(!unsafe.select(1, 1, SteamGridDBRole::horizontal_grid, {}, SteamGridDBOverride::override_native, secret) && cdn_calls == 0, "unapproved asset host was contacted");
  SteamGridDB corrupt(root / "corrupt", [&](const HttpRequest& request) {
    if (request.url.find("/api/v2/") != std::string::npos) return success(json::array({asset(1)}));
    return HttpResponse{true, 200, "not an image", secret};
  });
  require(!corrupt.select(1, 1, SteamGridDBRole::horizontal_grid, {}, SteamGridDBOverride::override_native, secret), "corrupt image bytes were accepted");
  bool offline = false;
  SteamGridDB stale(root / "stale", [&](const HttpRequest&) { return offline ? HttpResponse{false, 429, secret, secret} : success(json::array({{{"id", 10}, {"name", "Cached Game"}}})); }, [&] { return now; });
  require(bool(stale.search("Cached Game", secret)), "stale cache seed request failed");
  now += 86401; offline = true;
  const auto fallback = stale.search("Cached Game", secret);
  require(bool(fallback) && fallback.from_cache && fallback.stale && fallback.message.find(secret) == std::string::npos, "expired query should remain available as explicit stale offline fallback");
  no_secret(root);
}
void image_formats(const fs::path& root) {
  const auto check = [&](const std::string& name, const std::string& mime, const std::string& bytes, unsigned width, unsigned height, SteamGridDBRole role, const std::string& family) {
    SteamGridDB client(root / name, [&](const HttpRequest& request) {
      if (request.url.find("/api/v2/") != std::string::npos) { auto entry = asset(1, width, height, "https://cdn2.steamgriddb.com/" + family + "/fixture." + name); entry["mime"] = mime; return success(json::array({entry})); }
      return HttpResponse{true, 200, bytes, {}};
    });
    const auto selected = client.select(1, 1, role, {}, SteamGridDBOverride::override_native, secret);
    require(bool(selected) && selected.width == width && selected.height == height && read(selected.path) == bytes, "valid native JPEG/WebP dimensions were not preserved");
  };
  check("jpg", "image/jpeg", jpeg(), 460, 215, SteamGridDBRole::horizontal_grid, "grid");
  check("webp", "image/webp", webp(), 1, 1, SteamGridDBRole::icon, "icon");
  auto bad_png = png(); bad_png[29] = static_cast<char>(bad_png[29] ^ 1);
  SteamGridDB checksum(root / "bad-header-checksum", [&](const HttpRequest& request) {
    return request.url.find("/api/v2/") != std::string::npos ? success(json::array({asset(1)})) : HttpResponse{true, 200, bad_png, {}};
  });
  require(!checksum.select(1, 1, SteamGridDBRole::horizontal_grid, {}, SteamGridDBOverride::override_native, secret), "invalid PNG header checksum must be rejected before rendering");
  SteamGridDB mismatch(root / "mismatch", [&](const HttpRequest& request) {
    return request.url.find("/api/v2/") != std::string::npos ? success(json::array({asset(1, 920, 430)})) : HttpResponse{true, 200, png(), {}};
  });
  require(!mismatch.select(1, 1, SteamGridDBRole::horizontal_grid, {}, SteamGridDBOverride::override_native, secret), "downloaded dimensions must match the selected API asset");
  unsigned downloads = 0; std::uint64_t now = 3000000;
  SteamGridDB limited(root / "cdn-rate", [&](const HttpRequest& request) {
    if (request.url.find("/api/v2/") != std::string::npos) return success(json::array({asset(1)}));
    ++downloads; return HttpResponse{false, 429, secret, secret};
  }, [&] { return now; });
  (void)limited.select(1, 1, SteamGridDBRole::horizontal_grid, {}, SteamGridDBOverride::override_native, secret);
  (void)limited.select(1, 1, SteamGridDBRole::horizontal_grid, {}, SteamGridDBOverride::override_native, secret);
  require(downloads == 1, "rate-limited CDN asset must not be fetched redundantly during backoff");
  no_secret(root);
}
void roles_and_bounds(const fs::path& root) {
  std::vector<HttpRequest> requests;
  SteamGridDB roles(root / "roles", [&](const HttpRequest& request) {
    requests.push_back(request);
    if (request.url.find("/logos/game/") != std::string::npos) return success(json::array({asset(1, 460, 215, "https://cdn2.steamgriddb.com/logo/fixture.png")}));
    if (request.url.find("/heroes/game/") != std::string::npos) return success(json::array({asset(2, 1920, 620, "https://cdn2.steamgriddb.com/hero/fixture.png")}));
    return HttpResponse{true, 200, png(), {}};
  });
  const auto logo = roles.assets(1, SteamGridDBRole::logo, secret);
  const auto hero = roles.assets(1, SteamGridDBRole::hero, secret);
  require(bool(logo) && bool(hero) && logo.assets.front().role == SteamGridDBRole::logo && hero.assets.front().role == SteamGridDBRole::hero && requests.size() == 2, "logo and hero metadata must remain distinct provider/cache roles");
  require(bool(roles.select(1, 1, SteamGridDBRole::logo, {}, SteamGridDBOverride::override_native, secret)), "explicit logo selection failed");
  require(!roles.select(1, 2, SteamGridDBRole::hero, {}, SteamGridDBOverride::override_native, secret) && requests.back().max_bytes == 32u * 1024u * 1024u, "hero downloads require their own 32 MiB limit and actual native dimensions");
  unsigned calls = 0;
  SteamGridDB invalid_key(root / "invalid-key", [&](const HttpRequest&) { ++calls; return success(json::array()); });
  require(!invalid_key.search("Fixture", secret + "\r\nInjected: header") && calls == 0, "invalid bearer material must not be passed to a transport");
  SteamGridDB oversized_metadata(root / "oversized-metadata", [](const HttpRequest&) { return HttpResponse{true, 200, std::string(2u * 1024u * 1024u + 1, 'x'), {}}; });
  require(!oversized_metadata.search("Fixture", secret), "transport-returned metadata beyond the request limit must be rejected");
  SteamGridDB oversized_image(root / "oversized-image", [](const HttpRequest& request) {
    return request.url.find("/api/v2/") != std::string::npos ? success(json::array({asset(1)})) : HttpResponse{true, 200, std::string(16u * 1024u * 1024u + 1, 'x'), {}};
  });
  require(!oversized_image.select(1, 1, SteamGridDBRole::horizontal_grid, {}, SteamGridDBOverride::override_native, secret), "transport-returned artwork beyond the image limit must be rejected");
  json nested = json::array(); for (unsigned i = 0; i < 40; ++i) nested = json::array({std::move(nested)});
  SteamGridDB deep(root / "deep", [&](const HttpRequest&) { return success(json::array({{{"id", 1}, {"name", "Fixture"}, {"nested", nested}}})); });
  require(!deep.search("Fixture", secret), "excessive provider JSON nesting must be rejected before cache parsing");
  no_secret(root);
}
void pagination(const fs::path& root) {
  std::uint64_t now=4000000;unsigned calls=0;bool offline=false;
  const auto page=[](unsigned accepted,unsigned raw) {auto data=json::array();for(unsigned i=0;i<raw;++i)data.push_back(i<accepted?asset(i+1):asset(i+1,600,900));return data;};
  HttpTransport transport=[&](const HttpRequest& request) {
    ++calls;if(offline)return HttpResponse{false,503,{},{}};
    if(request.url.find("/search/")!=std::string::npos)return success(json::array({{{"id",1},{"name","Legacy"}}}));
    if(request.url.find("page=1")!=std::string::npos)return success(json::array({asset(1000)}));
    if(request.url.find("/game/43?")!=std::string::npos)return success(page(0,50));
    if(request.url.find("/game/44?")!=std::string::npos)return success(page(48,49));
    return success(page(49,50));
  };
  const auto cache=root/"cache";SteamGridDB client(cache,transport,[&]{return now;});
  const auto first=client.assets(42,SteamGridDBRole::horizontal_grid,secret);
  require(bool(first)&&first.assets.size()==49&&first.has_more,"a filtered full provider page hid the next asset page");
  const auto next=first.has_more?client.assets(42,SteamGridDBRole::horizontal_grid,secret,1):SteamGridDBAssetsResult{};
  require(bool(next)&&next.page==1&&next.assets.size()==1&&next.assets.front().id==1000&&!next.has_more,"the next provider page was not reachable after filtering");
  const auto all_filtered=client.assets(43,SteamGridDBRole::horizontal_grid,secret);
  require(bool(all_filtered)&&all_filtered.assets.empty()&&all_filtered.has_more,"a fully filtered full provider page hid later valid assets");
  require(client.assets(43,SteamGridDBRole::horizontal_grid,secret,1).assets.size()==1,"valid assets after an empty filtered page were unreachable");
  const auto short_page=client.assets(44,SteamGridDBRole::horizontal_grid,secret);
  require(bool(short_page)&&short_page.assets.size()==48&&!short_page.has_more,"a short raw provider page incorrectly offered another page");
  const auto cached_file=[&](std::string_view key) {for(const auto& entry:fs::directory_iterator(cache))if(entry.path().extension()==".json"&&json::parse(read(entry.path())).value("key","")==key)return entry.path();throw std::runtime_error("pagination cache fixture missing");};
  const auto first_file=cached_file("grids/42/0");const auto stored=json::parse(read(first_file));
  require(stored.value("version",0)==2&&stored.at("raw_count")==50&&stored.at("data").size()==49,"cache did not preserve raw provider page cardinality beside filtered assets");
  const auto before=calls;SteamGridDB restarted(cache,transport,[&]{return now;});
  const auto fresh=restarted.assets(42,SteamGridDBRole::horizontal_grid);
  require(bool(fresh)&&fresh.from_cache&&!fresh.stale&&fresh.has_more&&fresh.assets.size()==49&&calls==before,"fresh offline cache lost raw-page pagination metadata");
  const auto fresh_next=restarted.assets(42,SteamGridDBRole::horizontal_grid,{},1);
  require(bool(fresh_next)&&fresh_next.assets.size()==1&&fresh_next.assets.front().id==1000&&calls==before,"fresh cached next-page choice was unreachable offline");
  require(restarted.assets(43,SteamGridDBRole::horizontal_grid).has_more,"empty filtered page lost pagination after cache reload");
  now+=86401;offline=true;
  const auto stale=restarted.assets(42,SteamGridDBRole::horizontal_grid,secret);
  require(bool(stale)&&stale.from_cache&&stale.stale&&stale.has_more&&stale.assets.size()==49,"failed refresh lost stale pagination metadata");
  const auto stale_empty=restarted.assets(43,SteamGridDBRole::horizontal_grid,secret);
  require(bool(stale_empty)&&stale_empty.from_cache&&stale_empty.stale&&stale_empty.assets.empty()&&stale_empty.has_more,"fully filtered stale provider page lost its next page");
  const auto attempts=calls;const auto offline_page=restarted.assets(42,SteamGridDBRole::horizontal_grid);
  require(bool(offline_page)&&offline_page.from_cache&&offline_page.stale&&offline_page.has_more&&calls==attempts,"credential-free stale cache lost pagination or performed I/O");
  require(restarted.assets(43,SteamGridDBRole::horizontal_grid).has_more&&calls==attempts,"fully filtered credential-free stale page lost pagination");
  auto legacy=stored;legacy["version"]=1;legacy["fetched_at"]=now;legacy.erase("raw_count");{std::ofstream output(first_file);output<<legacy.dump();}
  const auto legacy_page=restarted.assets(42,SteamGridDBRole::horizontal_grid);
  require(bool(legacy_page)&&legacy_page.from_cache&&legacy_page.stale&&legacy_page.has_more&&legacy_page.assets.size()==49,"legacy filtered asset cache lost offline choices or guessed that pagination ended");
  offline=false;const auto refreshed=restarted.assets(42,SteamGridDBRole::horizontal_grid,secret);
  require(bool(refreshed)&&!refreshed.from_cache&&refreshed.has_more&&json::parse(read(first_file)).at("raw_count")==50,"legacy asset cache did not refresh into cardinality-aware format");
  require(bool(client.search("Legacy",secret)),"legacy search cache fixture could not be seeded");const auto search_file=cached_file("search/legacy");auto legacy_search=json::parse(read(search_file));legacy_search["version"]=1;legacy_search.erase("raw_count");{std::ofstream output(search_file);output<<legacy_search.dump();}
  const auto cached_search=restarted.search("Legacy");require(bool(cached_search)&&cached_search.from_cache&&!cached_search.stale,"cache migration unnecessarily discarded a valid legacy offline search");
  for(const auto& invalid:std::vector<json>{0,-1,51,true,"50"}) {auto corrupt=stored;corrupt["raw_count"]=invalid;{std::ofstream output(first_file);output<<corrupt.dump();}require(!restarted.assets(42,SteamGridDBRole::horizontal_grid),"incoherent cached raw cardinality was accepted");}
  no_secret(root);
}
}
int main() {
  const auto root = fs::weakly_canonical(fs::temp_directory_path()) / ("openxmb-sgdb-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try { provider(root / "provider"); failures(root / "failures"); image_formats(root / "formats"); roles_and_bounds(root / "bounds"); pagination(root / "pagination"); fs::remove_all(root); std::cout << "SteamGridDB selection, cache and native-priority checks passed\n"; }
  catch (const std::exception& error) { fs::remove_all(root); std::cerr << error.what() << '\n'; return 1; }
}
