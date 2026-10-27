#include "openxmb/themes/library.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <zlib.h>

namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;
using Bytes = std::vector<unsigned char>;
using namespace openxmb::themes;
unsigned checks = 0;
void require(bool condition, const char* message) { ++checks; if (!condition) throw std::runtime_error(message); }
template<class F> void rejected(F&& function, const char* message) {
  bool failed = false;
  try { function(); } catch (const std::exception&) { failed = true; }
  require(failed, message);
}
void put(const fs::path& path, const std::string& data) { fs::create_directories(path.parent_path()); std::ofstream(path, std::ios::binary) << data; }
void put(const fs::path& path, const Bytes& data) { fs::create_directories(path.parent_path()); std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(data.data()), data.size()); }
void word(Bytes& data, std::size_t at, std::uint32_t value, bool big = true) {
  if (data.size() < at + 4) data.resize(at + 4);
  for (unsigned i = 0; i < 4; ++i) data[at + i] = value >> (big ? 24 - i * 8 : i * 8);
}
Bytes compress(const Bytes& bytes) { uLongf size = compressBound(bytes.size()); Bytes out(size); require(compress2(out.data(), &size, bytes.data(), bytes.size(), 6) == Z_OK, "fixture compression"); out.resize(size); return out; }
Bytes gim() {
  Bytes out(84);
  out[0] = 'M'; out[1] = 'I'; out[2] = 'G'; out[3] = '.';
  out[16] = 4; word(out, 20, 68, false); word(out, 28, 16, false);
  out[36] = 3; out[40] = 1; out[42] = 1;
  word(out, 60, 48, false); word(out, 64, 52, false);
  out[80] = 100; out[81] = 150; out[82] = 250; out[83] = 255;
  return out;
}
Bytes p3t(bool cyclic = false) {
  Bytes strings;
  const auto str = [&](const std::string& value) { auto offset = strings.size(); strings.insert(strings.end(), value.begin(), value.end()); strings.push_back(0); return offset; };
  const auto root = str("theme"), info = str("info"), icon = str("icon"), name = str("name"), id = str("id"), src = str("src");
  const auto title = str("Native Test Theme"), icon_id = str("icon_game");
  const auto pixels = compress(gim());
  Bytes tree(132);
  word(tree, 0, root); word(tree, 4, 0); word(tree, 16, 0xffffffff); word(tree, 20, 28);
  word(tree, 28, info); word(tree, 32, 1); word(tree, 44, 72); word(tree, 48, 0xffffffff);
  word(tree, 56, name); word(tree, 60, 3); word(tree, 64, title);
  word(tree, 72, icon); word(tree, 76, 2); word(tree, 88, cyclic ? 72 : 0xffffffff); word(tree, 92, 0xffffffff);
  word(tree, 100, id); word(tree, 104, 3); word(tree, 108, icon_id);
  word(tree, 116, src); word(tree, 120, 6); word(tree, 124, 0); word(tree, 128, pixels.size());
  Bytes out(56); out[0] = 'P'; out[1] = '3'; out[2] = 'T'; out[3] = 'F';
  word(out, 8, 56); word(out, 12, tree.size());
  word(out, 16, 56 + tree.size()); word(out, 20, 0);
  word(out, 24, 56 + tree.size()); word(out, 28, strings.size());
  word(out, 32, 56 + tree.size()); word(out, 36, 0); word(out, 40, 56 + tree.size()); word(out, 44, 0);
  word(out, 48, 56 + tree.size() + strings.size()); word(out, 52, pixels.size());
  out.insert(out.end(), tree.begin(), tree.end()); out.insert(out.end(), strings.begin(), strings.end()); out.insert(out.end(), pixels.begin(), pixels.end());
  return out;
}
void malicious_zip(const fs::path& path, const char* name, bool link = false) {
  auto* archive = archive_write_new(); archive_write_set_format_zip(archive);
  require(archive_write_open_filename(archive, path.string().c_str()) == ARCHIVE_OK, "create archive fixture");
  const auto entry = [&](const char* pathname, const std::string& contents, bool symlink) {
    auto* item = archive_entry_new(); archive_entry_set_pathname(item, pathname); archive_entry_set_perm(item, 0644);
    archive_entry_set_filetype(item, symlink ? AE_IFLNK : AE_IFREG);
    if (symlink) archive_entry_set_symlink(item, "../../escape");
    archive_entry_set_size(item, symlink ? 0 : contents.size());
    archive_write_header(archive, item); if (!symlink) archive_write_data(archive, contents.data(), contents.size());
    archive_entry_free(item);
  };
  entry("theme.json", R"({"name":"Unsafe","settings":{"backgroundColor":"#ffffff"}})", false);
  entry(name, "unsafe", link);
  archive_write_close(archive); archive_write_free(archive);
}
}

int main() {
  const auto root = fs::temp_directory_path() / ("openxmb-theme-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{root};
  try {
    ThemeLibrary library(root / "library");
    library.set_baseline({{"background-type", "original"}, {"icon-color", "#ffffff"}});
    put(root / "fixture.p3t", p3t());
    const auto fixture_manifest = import_p3t(root / "fixture.p3t", root / "fixture");
    std::ifstream fixture(root / "fixture" / "icons" / "icon_game.png", std::ios::binary);
    const Bytes image((std::istreambuf_iterator<char>(fixture)), std::istreambuf_iterator<char>());
    const auto pack = root / "source";
    put(pack / "icons" / "icon_game.png", image);
    put(pack / "sounds" / "cursor.ogg", "sound fixture");
    put(pack / "frame-a.jpg", image); put(pack / "frame-b.jpg", image);
    const json manifest = {{"name", "Blue Theme"}, {"author", "Test"}, {"background", "frame-a.jpg"},
      {"settings", {{"backgroundColor", "#123456"}, {"iconColor", "#abcdef"}}},
      {"animation", {{"frames", {{{"file", "frame-a.jpg"}, {"duration_ms", 1000}}, {{"file", "frame-b.jpg"}, {"duration_ms", 2000}}}},
        {"layers", {{{"file", "frame-a.jpg"}, {"duration", 2.0}, {"keys", {{{"time", 0}, {"x", 0}, {"opacity", 0}}, {{"time", 2}, {"x", 1}, {"opacity", 1}}}}}}}}}};
    put(pack / "theme.json", manifest.dump());
    const auto imported = library.import_theme(pack);
    require(imported.name == "Blue Theme", "theme metadata imported");
    require(imported.icons.size() == 1 && imported.sounds.size() == 1, "theme resource discovery");
    require(fs::exists(pack / "theme.json"), "theme source left intact");
    require(library.discover().size() == 1, "theme discovery");
    ComponentOptions options; options.icons = false;
    auto applied = library.apply(imported.id, options);
    require(applied.icons.empty() && applied.sounds.contains("cursor") && applied.sounds.contains("cursor_up"), "component switches resolve independently and sound roles are mapped");
    require(applied.shell.at("background-color") == "#123456", "theme color applied");
    require(applied.shell.at("background-image") == imported.background.string(), "theme paths resolved");
    ThemeLibrary restarted(root / "library");
    require(restarted.active().has_value() && !restarted.active()->components.icons, "active theme and component switches survive restart");
    auto frame = ThemeLibrary::sample_animation(imported, 1.5);
    require(frame.background.filename() == "frame-b.jpg", "animation frame timing");
    require(frame.layers.size() == 1 && std::abs(frame.layers[0].x - 0.75F) < 0.001F && std::abs(frame.layers[0].opacity - 0.75F) < 0.001F, "animation keyframes interpolate");
    require(ThemeLibrary::sample_animation(applied, 3.5).background.filename() == "frame-a.jpg", "resolved animation loops");
    library.save_preset("No Icons", imported.id, options);
    require(library.presets() == std::vector<std::string>{"No Icons"}, "preset listed");
    require(!library.load_preset("No Icons").components.icons, "preset applies component selection");
    ComponentOptions no_colors; no_colors.colors = false;
    require(library.apply(imported.id, no_colors).shell.at("icon-color") == "#ffffff", "disabled components restore the pre-theme baseline");
    const auto exported = root / "shared.zip";
    library.export_theme(imported.id, exported);
    ThemeLibrary recipient(root / "recipient");
    auto round_trip = recipient.import_theme(exported);
    require(round_trip.name == imported.name && round_trip.icons.size() == imported.icons.size(), "ZIP export/import round trip");
    require(round_trip.dynamic, "animation survives export/import");
    auto captured = library.create_custom_theme("My Shared Appearance", {{"background-type", "image"}, {"background-image", imported.background.string()}, {"icon-color", "#fedcba"}});
    require(captured.name == "My Shared Appearance" && captured.settings.at("icon-color") == "#fedcba", "current appearance can be captured as a custom theme");
    require(!captured.background.empty() && captured.background.string().starts_with(captured.directory.string()), "custom theme capture copies background into portable resources");
    library.export_theme(captured.id, root / "captured.zip");
    require(recipient.import_theme(root / "captured.zip").settings.at("icon-color") == "#fedcba", "captured appearance survives portable ZIP sharing");
    put(root / "playlist" / "a.png", image); put(root / "playlist" / "b.png", image);
    put(root / "playlist" / "theme.json", json{{"name", "Playlist Theme"}, {"settings", {{"bgShuffle", "pl:Pictures"}, {"bgInterval", 2},
      {"bgPlaylists", {{{"name", "Pictures"}, {"items", {"file:a.png", "file:b.png"}}}}}}}}.dump());
    const auto playlist_theme = library.import_theme(root / "playlist");
    require(ThemeLibrary::sample_animation(playlist_theme, 2.5).background.filename() == "b.png", "MaXMB custom background playlists are playable native animations");
    rejected([&] { library.export_theme(imported.id, exported); }, "export does not overwrite existing files");
    auto reverted = restarted.revert();
    require(reverted.reverted && reverted.shell.at("background-type") == "original" && !restarted.active(), "revert restores persisted baseline");
    const auto malicious = root / "escape.zip";
    malicious_zip(malicious, "../../escape");
    rejected([&] { library.import_theme(malicious); }, "archive traversal rejected");
    require(!fs::exists(root / "escape"), "archive writes remain contained");
    malicious_zip(root / "linked.zip", "linked", true);
    rejected([&] { library.import_theme(root / "linked.zip"); }, "archive symbolic links rejected");
    put(root / "bad-path" / "theme.json", R"({"name":"Escape","background":"file:../../outside.jpg"})");
    rejected([&] { library.import_theme(root / "bad-path"); }, "manifest resource traversal rejected");
    put(root / "invalid" / "theme.json", "invalid JSON");
    rejected([&] { library.import_theme(root / "invalid"); }, "invalid JSON rejected");
    put(root / "missing" / "theme.json", R"({"name":"Missing","background":"gone.png"})");
    rejected([&] { library.import_theme(root / "missing"); }, "missing background rejects import before apply");
    auto huge = image; word(huge, 16, 16384); put(root / "huge-image" / "giant.png", huge);
    put(root / "huge-image" / "theme.json", R"({"name":"Huge","background":"giant.png"})");
    rejected([&] { library.import_theme(root / "huge-image"); }, "image allocation dimensions are bounded before decode");
    put(library.inbox() / "drop-in" / "theme.json", R"({"name":"Inbox","settings":{"backgroundColor":"#102030"}})");
    const auto inbox_theme = library.import_theme(library.inbox() / "drop-in");
    require(inbox_theme.name == "Inbox", "controller inbox import accepts owned inbox source folders");
    library.apply(imported.id);
    library.apply(imported.id, options);
    put(library.root() / "active.json", "corrupt interrupted state");
    require(library.active().has_value(), "corrupt primary settings recover from last-good backup");
    library.apply(imported.id, options);
    ImportLimits limits; limits.file_bytes = 2;
    ThemeLibrary restricted(root / "restricted", limits);
    rejected([&] { restricted.import_theme(pack); }, "oversized resources rejected");
    const auto static_theme = root / "static.p3t";
    put(static_theme, p3t());
    const auto p3t_import = library.import_theme(static_theme);
    require(p3t_import.name == "Native Test Theme" && p3t_import.icons.size() == 1, "real P3TF binary tree and GIM icon decode");
    std::ifstream png(p3t_import.icons.at("icon_game.png"), std::ios::binary);
    std::array<unsigned char, 8> signature{}; png.read(reinterpret_cast<char*>(signature.data()), signature.size());
    require(signature == std::array<unsigned char, 8>{137,80,78,71,13,10,26,10}, "GIM import writes a PNG icon");
    put(root / "cycle.p3t", p3t(true));
    rejected([&] { library.import_theme(root / "cycle.p3t"); }, "cyclic P3TF tree rejected");
    auto truncated = p3t(); truncated.resize(70); put(root / "truncated.p3t", truncated);
    rejected([&] { library.import_theme(root / "truncated.p3t"); }, "truncated P3TF rejected");
    require(library.discover().size() == 5, "failed imports leave existing library intact");
    const auto scripted_source = fs::path(__FILE__).parent_path() / "fixtures/native-scripted.p3t";
    const auto scripted = library.import_theme(scripted_source);
    require(scripted.dynamic && scripted.warnings.empty(), "scripted P3TF fixture imports real RAF resources without fallback");
    const auto scripted_applied = library.apply(scripted.id);
    const auto initial_scene = ThemeLibrary::sample_animation(scripted_applied, 0);
    const auto moving_scene = ThemeLibrary::sample_animation(scripted_applied, 0.7);
    require(initial_scene.draws.size() == 1 && moving_scene.draws.size() == 1 && moving_scene.warnings.empty(), "binary P3TF/RAFO/VSMX pipeline produces live textured geometry");
    require(std::abs(initial_scene.draws[0].vertices[0].x - moving_scene.draws[0].vertices[0].x) > 0.01F, "imported theme script animates projected native vertices");
    ComponentOptions no_background; no_background.background = false;
    require(ThemeLibrary::sample_animation(library.apply(scripted.id, no_background), 0).draws.empty(), "RAF background component can be disabled independently of overlays");
    std::cout << "Theme library: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "Theme library failure: " << error.what() << '\n'; return 1; }
}
