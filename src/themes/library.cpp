#include "openxmb/themes/library.hpp"
#include "openxmb/themes/raf_runtime.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <system_error>
#include <zlib.h>

namespace openxmb::themes {
namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;

std::string lower(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) { return std::tolower(c); });
  return value;
}

std::string slug(std::string_view value) {
  std::string out;
  for (unsigned char c : value) {
    if (out.size() >= 64) break;
    if (std::isalnum(c) && c < 128) out += static_cast<char>(std::tolower(c));
    else if (!out.empty() && out.back() != '-') out += '-';
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? "theme" : out;
}

fs::path relative_resource(std::string value) {
  if (value.starts_with("file:")) value.erase(0, 5);
  std::replace(value.begin(), value.end(), '\\', '/');
  if (value.empty() || value.size() > 1024 || value.find('\0') != std::string::npos || value.find(':') != std::string::npos)
    throw ThemeError("Invalid theme resource path");
  fs::path result(value);
  if (result.is_absolute() || result.has_root_name()) throw ThemeError("Absolute theme resource path rejected");
  for (const auto& part : result) {
    if (part == ".." || part == "." || part.empty()) throw ThemeError("Theme resource escapes its directory");
    const auto text = part.string();
    if (text.back() == '.' || text.back() == ' ' || std::ranges::any_of(text, [](unsigned char c) { return c < 32; }))
      throw ThemeError("Theme resource contains an unsafe filename");
    const auto stem = lower(part.stem().string());
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
        (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' && stem[3] <= '9'))
      throw ThemeError("Theme resource contains a reserved device filename");
  }
  return result;
}

bool contained(const fs::path& root, const fs::path& candidate) {
  const auto base = fs::weakly_canonical(root);
  const auto path = fs::weakly_canonical(candidate);
  auto [a, b] = std::mismatch(base.begin(), base.end(), path.begin(), path.end());
  return a == base.end();
}

fs::path resource(const fs::path& directory, const json& value, bool required = false) {
  if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
    if (required) throw ThemeError("Missing theme resource");
    return {};
  }
  const auto path = directory / relative_resource(value.get<std::string>());
  if (!contained(directory, path) || fs::is_symlink(path)) throw ThemeError("Theme resource escapes its directory");
  if (!fs::is_regular_file(path)) {
    if (required) throw ThemeError("Theme resource is missing: " + path.filename().string());
    return {};
  }
  return path;
}

json read_json(const fs::path& path, std::uint64_t limit = 2 * 1024 * 1024) {
  if (!fs::is_regular_file(path) || fs::file_size(path) > limit) throw ThemeError("Invalid or oversized theme metadata");
  std::ifstream stream(path, std::ios::binary);
  try { return json::parse(stream); }
  catch (const json::exception&) { throw ThemeError("Theme metadata is not valid JSON"); }
}

void validate_image(const fs::path& path, const ImportLimits& limits) {
  const auto extension = lower(path.extension().string());
  if (extension != ".png" && extension != ".jpg" && extension != ".jpeg" && extension != ".gif" && extension != ".bmp" && extension != ".webp") return;
  std::ifstream stream(path, std::ios::binary);
  std::vector<unsigned char> bytes(static_cast<std::size_t>(std::min<std::uint64_t>(fs::file_size(path), 1024 * 1024)));
  stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  const auto u16 = [&](std::size_t at, bool big = false) -> unsigned {
    if (at > bytes.size() || bytes.size() - at < 2) throw ThemeError("Truncated theme image header");
    return big ? (bytes[at] << 8) | bytes[at + 1] : bytes[at] | (bytes[at + 1] << 8);
  };
  const auto u32 = [&](std::size_t at, bool big = false) -> std::uint32_t {
    if (at > bytes.size() || bytes.size() - at < 4) throw ThemeError("Truncated theme image header");
    return big ? (std::uint32_t(bytes[at]) << 24) | (std::uint32_t(bytes[at+1]) << 16) | (bytes[at+2] << 8) | bytes[at+3]
               : bytes[at] | (bytes[at+1] << 8) | (std::uint32_t(bytes[at+2]) << 16) | (std::uint32_t(bytes[at+3]) << 24);
  };
  const auto starts = [&](std::size_t offset, std::string_view magic) {
    return offset <= bytes.size() && magic.size() <= bytes.size() - offset &&
      std::equal(magic.begin(), magic.end(), bytes.begin() + offset, [](char a, unsigned char b) { return static_cast<unsigned char>(a) == b; });
  };
  std::uint32_t width = 0, height = 0;
  if (starts(0, std::string_view("\x89PNG\r\n\x1a\n", 8))) { width = u32(16, true); height = u32(20, true); }
  else if (starts(0, "GIF87a") || starts(0, "GIF89a")) { width = u16(6); height = u16(8); }
  else if (starts(0, "BM")) { width = u32(18); height = u32(22) & 0x7fffffff; }
  else if (starts(0, "RIFF") && starts(8, "WEBP")) {
    if (starts(12, "VP8X") && bytes.size() >= 30) {
      width = 1 + bytes[24] + (bytes[25] << 8) + (bytes[26] << 16);
      height = 1 + bytes[27] + (bytes[28] << 8) + (bytes[29] << 16);
    } else if (starts(12, "VP8 ") && starts(23, "\x9d\x01\x2a")) { width = u16(26) & 0x3fff; height = u16(28) & 0x3fff; }
    else if (starts(12, "VP8L") && bytes.size() >= 25 && bytes[20] == 0x2f) {
      width = 1 + bytes[21] + ((bytes[22] & 0x3f) << 8);
      height = 1 + (bytes[22] >> 6) + (bytes[23] << 2) + ((bytes[24] & 0xf) << 10);
    }
  } else if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xd8) {
    std::size_t offset = 2;
    while (offset + 4 <= bytes.size()) {
      if (bytes[offset++] != 0xff) continue;
      while (offset < bytes.size() && bytes[offset] == 0xff) ++offset;
      if (offset >= bytes.size()) break;
      const auto marker = bytes[offset++];
      if (marker == 0xd9 || marker == 0xda) break;
      if ((marker >= 0xd0 && marker <= 0xd7) || marker == 1) continue;
      const auto size = u16(offset, true);
      if (size < 2 || size > bytes.size() - offset) break;
      if ((marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) || (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf)) {
        height = u16(offset + 3, true); width = u16(offset + 5, true); break;
      }
      offset += size;
    }
  }
  if (!width || !height || width > limits.image_dimension || height > limits.image_dimension)
    throw ThemeError("Theme image is malformed or exceeds the " + std::to_string(limits.image_dimension) + "px dimension limit: " + path.filename().string());
}

void validate_tree(const fs::path& directory, const ImportLimits& limits) {
  std::uint64_t bytes = 0;
  std::size_t count = 0;
  for (const auto& entry : fs::recursive_directory_iterator(directory)) {
    if (++count > limits.entries) throw ThemeError("Theme contains too many resources");
    if (entry.is_symlink() || !contained(directory, entry.path())) throw ThemeError("Theme resource escapes its directory");
    if (entry.is_directory()) continue;
    if (!entry.is_regular_file()) throw ThemeError("Theme contains a special file");
    const auto size = entry.file_size();
    if (size > limits.file_bytes || (bytes += size) > limits.expanded_bytes) throw ThemeError("Theme exceeds its expanded resource budget");
    validate_image(entry.path(), limits);
  }
}

void atomic_json(const fs::path& path, const json& value) {
  fs::create_directories(path.parent_path());
  auto temporary = path;
  temporary += ".part";
  if (fs::is_symlink(path) || fs::is_symlink(temporary) || fs::is_symlink(path.string() + ".bak"))
    throw ThemeError("Refusing to overwrite a symbolic link");
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream << value.dump(2) << '\n';
    stream.flush();
    if (!stream) throw ThemeError("Could not save theme state");
  }
  // Keep a last-good copy so interrupted persistence can be recovered.
  if (fs::is_regular_file(path)) {
    try { (void)read_json(path); fs::copy_file(path, path.string() + ".bak", fs::copy_options::overwrite_existing); }
    catch (const ThemeError&) { /* Preserve the existing last-good backup. */ }
  }
  std::error_code error;
  fs::rename(temporary, path, error);
#ifdef _WIN32
  if (error && fs::exists(path)) {
    fs::remove(path);
    fs::rename(temporary, path, error);
  }
#endif
  if (error) throw ThemeError("Could not replace theme state: " + error.message());
}

json options_json(ComponentOptions o) {
  return {{"background", o.background}, {"icons", o.icons}, {"sounds", o.sounds},
          {"colors", o.colors}, {"font", o.font}, {"overlay", o.overlay}};
}

ComponentOptions options_from(const json& j) {
  ComponentOptions o;
  if (!j.is_object()) return o;
  const auto get = [&](const char* key) { return !j.contains(key) || !j[key].is_boolean() || j[key].get<bool>(); };
  o.background = get("background"); o.icons = get("icons"); o.sounds = get("sounds");
  o.colors = get("colors"); o.font = get("font"); o.overlay = get("overlay");
  return o;
}

std::string checksum(const fs::path& file, std::uint64_t maximum) {
  if (fs::file_size(file) > maximum) throw ThemeError("Theme archive exceeds the import size limit");
  std::ifstream input(file, std::ios::binary);
  std::array<char, 65536> buffer{};
  uLong crc = crc32(0, nullptr, 0);
  while (input) {
    input.read(buffer.data(), buffer.size());
    crc = crc32(crc, reinterpret_cast<const Bytef*>(buffer.data()), static_cast<uInt>(input.gcount()));
  }
  if (!input.eof()) throw ThemeError("Could not read theme archive");
  std::ostringstream result;
  result << std::hex << std::setw(8) << std::setfill('0') << crc;
  return result.str();
}

void extract_zip(const fs::path& source, const fs::path& destination, const ImportLimits& limits) {
  using Reader = std::unique_ptr<archive, decltype(&archive_read_free)>;
  Reader input(archive_read_new(), archive_read_free);
  archive_read_support_filter_none(input.get());
  archive_read_support_format_zip(input.get());
  if (archive_read_open_filename(input.get(), source.string().c_str(), 65536) != ARCHIVE_OK)
    throw ThemeError("Could not open theme ZIP archive");
  std::uint64_t total = 0;
  std::size_t count = 0;
  std::set<std::string> names;
  archive_entry* entry = nullptr;
  int status = ARCHIVE_OK;
  while ((status = archive_read_next_header(input.get(), &entry)) == ARCHIVE_OK) {
    if (++count > limits.entries) throw ThemeError("Theme contains too many archive entries");
    const char* pathname = archive_entry_pathname(entry);
    if (!pathname) throw ThemeError("Theme contains an unnamed entry");
    auto name = std::string(pathname);
    while (!name.empty() && name.back() == '/') name.pop_back();
    if (name.empty()) continue;
    const auto relative = relative_resource(name);
    if (!names.insert(lower(relative.generic_string())).second) throw ThemeError("Theme contains duplicate archive paths");
    if (archive_entry_symlink(entry) || archive_entry_hardlink(entry)) throw ThemeError("Theme links are not allowed");
    const auto output = destination / relative;
    if (!contained(destination, output)) throw ThemeError("Theme archive path escapes its directory");
    if (archive_entry_filetype(entry) == AE_IFDIR) { fs::create_directories(output); continue; }
    if (archive_entry_filetype(entry) != AE_IFREG) throw ThemeError("Theme archive contains a special file");
    if (archive_entry_is_encrypted(entry) == 1) throw ThemeError("Encrypted theme archives are not supported");
    const auto size = archive_entry_size(entry);
    if (size < 0 || static_cast<std::uint64_t>(size) > limits.file_bytes) throw ThemeError("Theme resource exceeds its size limit");
    fs::create_directories(output.parent_path());
    std::ofstream stream(output, std::ios::binary);
    std::array<char, 65536> buffer{};
    std::uint64_t written = 0;
    la_ssize_t bytes = 0;
    while ((bytes = archive_read_data(input.get(), buffer.data(), buffer.size())) > 0) {
      written += static_cast<std::uint64_t>(bytes);
      total += static_cast<std::uint64_t>(bytes);
      if (written > limits.file_bytes || total > limits.expanded_bytes) throw ThemeError("Theme expands beyond its resource budget");
      stream.write(buffer.data(), bytes);
      if (!stream) throw ThemeError("Could not extract theme resource");
    }
    if (bytes < 0 || written != static_cast<std::uint64_t>(size)) throw ThemeError("Corrupt theme archive resource");
  }
  if (status != ARCHIVE_EOF) throw ThemeError("Corrupt theme ZIP directory");
}

void copy_directory(const fs::path& source, const fs::path& destination, const ImportLimits& limits) {
  std::uint64_t bytes = 0;
  std::size_t entries = 0;
  for (const auto& entry : fs::recursive_directory_iterator(source)) {
    if (++entries > limits.entries) throw ThemeError("Theme contains too many files");
    if (entry.is_symlink()) throw ThemeError("Theme folders cannot contain symbolic links");
    const auto relative = entry.path().lexically_relative(source);
    const auto output = destination / relative_resource(relative.generic_string());
    if (entry.is_directory()) { fs::create_directories(output); continue; }
    if (!entry.is_regular_file()) throw ThemeError("Theme contains a special file");
    const auto size = entry.file_size();
    bytes += size;
    if (size > limits.file_bytes || bytes > limits.expanded_bytes) throw ThemeError("Theme exceeds its resource budget");
    fs::create_directories(output.parent_path());
    fs::copy_file(entry.path(), output);
  }
}

json normalized_manifest(const fs::path& directory) {
  json manifest;
  if (fs::is_regular_file(directory / "theme.json")) manifest = read_json(directory / "theme.json");
  else {
    // A dropped icon set may contain its own display-name JSON or no metadata.
    manifest = {{"name", directory.filename().string()}, {"icons", json::object()}};
    for (const auto& entry : fs::directory_iterator(directory)) {
      if (entry.is_regular_file() && lower(entry.path().extension().string()) == ".json") {
        auto metadata = read_json(entry.path());
        if (metadata.contains("name") && metadata["name"].is_string()) manifest["name"] = metadata["name"];
      }
    }
  }
  if (!manifest.is_object()) throw ThemeError("Theme manifest must be an object");
  manifest["format"] = "openxmb-theme";
  manifest["version"] = 1;
  if (!manifest.contains("settings")) manifest["settings"] = json::object();
  if (!manifest["settings"].is_object()) throw ThemeError("Theme settings must be an object");
  const auto& settings = manifest["settings"];
  if (!manifest.contains("background") && settings.contains("wallpaper")) manifest["background"] = settings["wallpaper"];
  if (!manifest.contains("font") && settings.contains("font")) manifest["font"] = settings["font"];
  if (!manifest.contains("icons") || !manifest["icons"].is_object()) manifest["icons"] = json::object();
  if (!manifest.contains("sounds") || !manifest["sounds"].is_object()) manifest["sounds"] = json::object();
  for (const auto& entry : fs::recursive_directory_iterator(directory)) {
    if (!entry.is_regular_file()) continue;
    const auto relative = entry.path().lexically_relative(directory).generic_string();
    const auto filename = lower(entry.path().filename().string());
    const auto extension = lower(entry.path().extension().string());
    if ((relative.starts_with("icons/") || relative.find('/') == std::string::npos) &&
        (filename.starts_with("icon_") || filename.starts_with("tex_")) &&
        (extension == ".png" || extension == ".jpg" || extension == ".webp" || extension == ".bmp"))
      manifest["icons"][entry.path().stem().string() + ".png"] = relative;
    const std::set<std::string> sound_roles{"cursor", "enter", "back", "coldboot", "error", "ok", "options"};
    if ((relative.starts_with("sounds/") || (relative.find('/') == std::string::npos && sound_roles.contains(lower(entry.path().stem().string())))) &&
        (extension == ".wav" || extension == ".ogg" || extension == ".mp3" || extension == ".flac"))
      manifest["sounds"][lower(entry.path().stem().string())] = relative;
    const std::map<std::string, std::string> players{{"music player", "mp"}, {"video player", "vp"}, {"photo viewer", "pv"}};
    auto parent = lower(entry.path().parent_path().filename().string());
    if (players.contains(parent) && (extension == ".png" || extension == ".jpg" || extension == ".webp" || extension == ".bmp"))
      manifest["icons"][players.at(parent) + ":" + lower(entry.path().stem().string()) + ".png"] = relative;
  }
  auto animation = manifest.value("animation", json::object());
  if (!animation.is_object()) throw ThemeError("Theme animation must be an object");
  const auto playlist = [&](const char* selector, const char* lists) {
    json items = json::array();
    if (!settings.contains(selector) || !settings[selector].is_string()) return items;
    const auto choice = settings[selector].get<std::string>();
    if (!choice.starts_with("pl:") || !settings.contains(lists) || !settings[lists].is_array()) return items;
    for (const auto& list : settings[lists])
      if (list.is_object() && list.value("name", std::string{}) == choice.substr(3) && list.contains("items") && list["items"].is_array()) return list["items"];
    return items;
  };
  const auto interval = [&](const char* field) {
    const double value = settings.contains(field) && settings[field].is_number() ? settings[field].get<double>() : 10;
    if (!std::isfinite(value) || value < 0.1 || value > 3600) throw ThemeError("Invalid theme playlist interval");
    return value;
  };
  const auto backgrounds = playlist("bgShuffle", "bgPlaylists");
  if (!backgrounds.empty() && !animation.contains("frames")) {
    animation["frames"] = json::array();
    const auto seconds = interval("bgInterval");
    for (const auto& item : backgrounds) {
      if (!item.is_string()) throw ThemeError("Invalid theme background playlist item");
      animation["frames"].push_back({{"file", item}, {"duration_ms", seconds * 1000}});
    }
  }
  const auto overlays = playlist("ovShuffle", settings.contains("ovPlaylists") ? "ovPlaylists" : "bgPlaylists");
  if (!overlays.empty() && !animation.contains("layers")) {
    animation["layers"] = json::array();
    const auto seconds = interval("ovInterval");
    for (std::size_t i = 0; i < overlays.size(); ++i) {
      if (!overlays[i].is_string()) throw ThemeError("Invalid theme overlay playlist item");
      animation["layers"].push_back({{"file", overlays[i]}, {"duration", seconds * overlays.size()},
          {"show_start", seconds * i}, {"show_end", seconds * (i + 1)}});
    }
  } else if (settings.contains("overlay") && settings["overlay"].is_string() && !settings["overlay"].get_ref<const std::string&>().empty() && !animation.contains("layers")) {
    animation["layers"] = {{{"file", settings["overlay"]}}};
  }
  if (!animation.empty()) manifest["animation"] = std::move(animation);
  if (manifest.value("name", std::string{}).empty()) manifest["name"] = "Imported Theme";
  const bool useful = !manifest["icons"].empty() || !manifest["sounds"].empty() || manifest.contains("background") ||
                     !settings.empty() || manifest.contains("animation");
  if (!useful) throw ThemeError("This folder does not contain a theme or icon pack");
  return manifest;
}

void validate_animation(const fs::path& directory, json& animation, const ImportLimits& limits) {
  if (!animation.is_object()) throw ThemeError("Theme animation must be an object");
  if (animation.value("kind", std::string{}) == "raf") {
    validate_raf_animation(animation, directory, limits);
    return;
  }
  if (animation.contains("frames")) {
    if (!animation["frames"].is_array() || animation["frames"].empty() || animation["frames"].size() > 512)
      throw ThemeError("Invalid animation frame count");
    for (auto& frame : animation["frames"]) {
      if (!frame.is_object()) throw ThemeError("Invalid animation frame");
      resource(directory, frame.value("file", json{}), true);
      const auto duration = frame.value("duration_ms", 1000.0);
      if (!std::isfinite(duration) || duration < 16 || duration > 3600000) throw ThemeError("Invalid animation duration");
    }
  }
  if (animation.contains("layers")) {
    if (!animation["layers"].is_array() || animation["layers"].size() > 128) throw ThemeError("Invalid animation layer count");
    for (auto& layer : animation["layers"]) {
      if (!layer.is_object()) throw ThemeError("Invalid animation layer");
      resource(directory, layer.value("file", json{}), true);
      for (const auto* field : {"x", "y", "width", "height", "opacity", "duration", "show_start", "show_end"})
        if (layer.contains(field) && (!layer[field].is_number() || !std::isfinite(layer[field].get<double>()) || std::abs(layer[field].get<double>()) > 86400))
          throw ThemeError("Invalid animation layer value");
      if (layer.value("duration", 0.0) < 0) throw ThemeError("Invalid animation loop duration");
      if (layer.contains("additive") && !layer["additive"].is_boolean()) throw ThemeError("Invalid animation blending option");
      if (layer.contains("keys")) {
        if (!layer["keys"].is_array() || layer["keys"].size() > 4096) throw ThemeError("Invalid animation key count");
        double previous = -1;
        for (const auto& key : layer["keys"]) {
          const double time = key.value("time", -1.0);
          if (!std::isfinite(time) || time < 0 || time < previous || time > 86400) throw ThemeError("Invalid animation key time");
          previous = time;
          for (const auto* field : {"x", "y", "width", "height", "opacity"})
            if (key.contains(field) && (!key[field].is_number() || !std::isfinite(key[field].get<double>()) || std::abs(key[field].get<double>()) > 10000))
              throw ThemeError("Invalid animation key value");
        }
      }
    }
  }
}

json path_map(const std::map<std::string, fs::path>& paths) {
  json out = json::object();
  for (const auto& [key, value] : paths) out[key] = value.string();
  return out;
}
} // namespace

std::string AppliedTheme::serialize() const {
  return json{{"id", id}, {"name", name}, {"directory", directory.string()}, {"shell", shell}, {"settings", settings}, {"icons", path_map(icons)},
              {"sounds", path_map(sounds)}, {"animation", animation}, {"warnings", warnings},
              {"components", options_json(components)}, {"reverted", reverted}}.dump();
}

ThemeLibrary::ThemeLibrary(fs::path root, ImportLimits limits) : root_(fs::absolute(std::move(root))), limits_(limits) {
  if (fs::is_symlink(root_)) throw ThemeError("Theme data root cannot be a symbolic link");
  for (const auto* folder : {"themes", "inbox", "exports", "presets"}) {
    if (fs::is_symlink(root_ / folder)) throw ThemeError("Managed theme directories cannot be symbolic links");
    fs::create_directories(root_ / folder);
  }
}
const fs::path& ThemeLibrary::root() const noexcept { return root_; }
fs::path ThemeLibrary::inbox() const { return root_ / "inbox"; }

ThemeInfo ThemeLibrary::load(const fs::path& directory) const {
  if (!contained(root_ / "themes", directory) || fs::is_symlink(directory)) throw ThemeError("Theme directory is outside the library");
  auto manifest = read_json(directory / "theme.json");
  ThemeInfo info;
  info.directory = directory;
  info.id = directory.filename().string();
  info.name = manifest.value("name", info.id);
  info.author = manifest.value("author", std::string{});
  info.description = manifest.value("description", manifest.value("comment", std::string{}));
  if (info.name.empty() || info.name.size() > 256 || info.author.size() > 512 || info.description.size() > 16384)
    throw ThemeError("Theme display metadata exceeds its size limit");
  info.settings = manifest.value("settings", json::object());
  if (!info.settings.is_object()) throw ThemeError("Invalid theme settings");
  info.background = resource(directory, manifest.value("background", json{}));
  if (manifest.contains("background") && manifest["background"].is_string() && !manifest["background"].get_ref<const std::string&>().empty() && info.background.empty())
    throw ThemeError("Theme background resource is missing");
  info.preview = resource(directory, manifest.value("preview", manifest.value("screenshot", json{})));
  if (info.preview.empty()) info.preview = info.background;
  info.font = resource(directory, manifest.value("font", json{}));
  if (manifest.contains("font") && manifest["font"].is_string() && !manifest["font"].get_ref<const std::string&>().empty() && info.font.empty())
    throw ThemeError("Theme font resource is missing");
  const auto collect = [&](const char* key, auto& out) {
    if (!manifest.contains(key)) return;
    if (!manifest[key].is_object() || manifest[key].size() > limits_.entries) throw ThemeError("Invalid theme resource map");
    for (const auto& [name, value] : manifest[key].items()) {
      if (name.size() > 128 || name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
        throw ThemeError("Invalid theme resource name");
      auto file = resource(directory, value, true);
      if (fs::file_size(file) > limits_.file_bytes) throw ThemeError("Oversized theme resource");
      out.emplace(name, std::move(file));
    }
  };
  collect("icons", info.icons); collect("sounds", info.sounds);
  info.animation = manifest.value("animation", json::object());
  validate_animation(directory, info.animation, limits_);
  info.dynamic = info.animation.value("kind", std::string{}) == "raf" ||
                 (info.animation.contains("frames") && !info.animation["frames"].empty()) ||
                 (info.animation.contains("layers") && !info.animation["layers"].empty());
  if (manifest.contains("warnings") && manifest["warnings"].is_array())
    for (const auto& warning : manifest["warnings"]) if (warning.is_string()) info.warnings.push_back(warning.get<std::string>());
  return info;
}

std::vector<ThemeInfo> ThemeLibrary::discover() const {
  std::vector<ThemeInfo> out;
  for (const auto& entry : fs::directory_iterator(root_ / "themes")) {
    if (!entry.is_directory() || entry.is_symlink() || entry.path().filename().string().starts_with('.')) continue;
    try { out.push_back(load(entry.path())); } catch (const std::exception&) { /* A corrupt pack must not hide good packs. */ }
  }
  std::ranges::sort(out, [](const auto& a, const auto& b) { return lower(a.name) < lower(b.name); });
  return out;
}

ThemeInfo ThemeLibrary::preview(const std::string& id) const {
  if (id.empty() || fs::path(id).filename() != id || id == "." || id == "..") throw ThemeError("Invalid theme identifier");
  return load(root_ / "themes" / id);
}

ThemeInfo ThemeLibrary::import_theme(const fs::path& source) {
  if (fs::is_symlink(source) || (!fs::is_regular_file(source) && !fs::is_directory(source))) throw ThemeError("Theme source does not exist or is a link");
  if (fs::is_directory(source) && (contained(source, root_) || contained(root_ / "themes", source)))
    throw ThemeError("Import a theme from outside its managed theme directory");
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto temporary = root_ / "themes" / (".import-" + suffix);
  fs::create_directory(temporary);
  try {
    json manifest;
    if (fs::is_directory(source)) copy_directory(source, temporary, limits_);
    else {
      const auto hash = checksum(source, limits_.archive_bytes);
      const auto extension = lower(source.extension().string());
      if (extension == ".p3t") manifest = import_p3t(source, temporary, limits_);
      else if (extension == ".zip") extract_zip(source, temporary, limits_);
      else throw ThemeError("Import a .p3t, .zip, or theme/icon-pack folder");
    }
    if (manifest.is_null()) {
      auto source_root = temporary;
      if (!fs::exists(temporary / "theme.json")) {
        std::vector<fs::path> candidates;
        for (const auto& entry : fs::directory_iterator(temporary))
          if (entry.is_directory() && fs::is_regular_file(entry.path() / "theme.json")) candidates.push_back(entry.path());
        if (candidates.size() == 1) source_root = candidates.front();
      }
      manifest = normalized_manifest(source_root);
      if (source_root != temporary) {
        const auto folder = source_root.filename();
        for (const auto& entry : fs::directory_iterator(source_root)) fs::rename(entry.path(), temporary / entry.path().filename());
        fs::remove(temporary / folder);
      }
    }
    auto id = slug(manifest.value("name", source.stem().string()));
    unsigned number = 2;
    const auto base = id;
    while (fs::exists(root_ / "themes" / id)) id = base + "-" + std::to_string(number++);
    manifest["id"] = id;
    atomic_json(temporary / "theme.json", manifest);
    validate_tree(temporary, limits_);
    // Validate every referenced resource before making an import visible.
    auto validated = load(temporary);
    const auto destination = root_ / "themes" / id;
    fs::rename(temporary, destination);
    return load(destination);
  } catch (...) {
    std::error_code ignored;
    fs::remove_all(temporary, ignored);
    throw;
  }
}

ThemeInfo ThemeLibrary::create_custom_theme(const std::string& name, json settings) {
  if (name.empty() || name.size() > 256 || !settings.is_object()) throw ThemeError("Enter a custom theme name");
  const auto temporary = root_ / (".capture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directory(temporary);
  try {
    json manifest = {{"format", "openxmb-theme"}, {"version", 1}, {"name", name}, {"settings", settings}};
    if (auto selected = active()) {
      const auto theme = preview(selected->id);
      copy_directory(theme.directory, temporary, limits_);
      auto saved = read_json(temporary / "theme.json");
      saved["name"] = name;
      saved["settings"] = settings;
      if (!selected->components.icons) {
        for (const auto& [key, file] : theme.icons) fs::remove(temporary / file.lexically_relative(theme.directory));
        saved.erase("icons");
      }
      if (!selected->components.sounds) {
        for (const auto& [key, file] : theme.sounds) fs::remove(temporary / file.lexically_relative(theme.directory));
        saved.erase("sounds");
      }
      if (!selected->components.font) saved.erase("font");
      if (!selected->components.overlay && saved.contains("animation")) saved["animation"].erase("layers");
      if (!selected->components.background) { saved.erase("background"); saved.erase("animation"); }
      manifest = std::move(saved);
    }
    const auto capture = [&](const char* key, const char* target, const char* folder) {
      if (!settings.contains(key) || !settings[key].is_string()) return;
      const fs::path source(settings[key].get<std::string>());
      if (source.empty() || source == "default" || source == "auto") return;
      if (!fs::is_regular_file(source) || fs::is_symlink(source) || fs::file_size(source) > limits_.file_bytes)
        throw ThemeError("The current theme's selected resource cannot be shared");
      const auto relative = fs::path(folder) / (std::string(target) + source.extension().string());
      fs::create_directories((temporary / relative).parent_path());
      fs::copy_file(source, temporary / relative, fs::copy_options::overwrite_existing);
      manifest[target] = relative.generic_string();
      manifest["settings"][key] = "file:" + relative.generic_string();
    };
    capture("background-image", "background", "media");
    capture("font-path", "font", "fonts");
    atomic_json(temporary / "theme.json", manifest);
    auto imported = import_theme(temporary);
    fs::remove_all(temporary);
    return imported;
  } catch (...) { std::error_code ignored; fs::remove_all(temporary, ignored); throw; }
}

void ThemeLibrary::export_theme(const std::string& id, const fs::path& destination) const {
  const auto info = preview(id);
  if (fs::exists(destination)) throw ThemeError("Export destination already exists; choose a new filename");
  if (destination.empty() || contained(info.directory, destination)) throw ThemeError("Invalid theme export destination");
  if (!destination.parent_path().empty()) fs::create_directories(destination.parent_path());
  const auto partial = fs::path(destination.string() + ".part");
  if (fs::exists(partial) || fs::is_symlink(partial)) throw ThemeError("Export is already in progress");
  using Writer = std::unique_ptr<archive, decltype(&archive_write_free)>;
  Writer output(archive_write_new(), archive_write_free);
  archive_write_set_format_zip(output.get());
  if (archive_write_open_filename(output.get(), partial.string().c_str()) != ARCHIVE_OK) throw ThemeError("Could not create theme export");
  try {
    std::uint64_t bytes = 0;
    std::size_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(info.directory)) {
      if (entry.is_symlink()) throw ThemeError("Cannot export a linked theme resource");
      if (!entry.is_regular_file() || entry.path().extension() == ".bak") continue;
      if (++count > limits_.entries || entry.file_size() > limits_.file_bytes || (bytes += entry.file_size()) > limits_.expanded_bytes)
        throw ThemeError("Theme export exceeds its resource budget");
      auto relative = entry.path().lexically_relative(info.directory).generic_string();
      std::unique_ptr<archive_entry, decltype(&archive_entry_free)> item(archive_entry_new(), archive_entry_free);
      archive_entry_set_pathname(item.get(), relative.c_str());
      archive_entry_set_filetype(item.get(), AE_IFREG);
      archive_entry_set_perm(item.get(), 0644);
      archive_entry_set_size(item.get(), static_cast<la_int64_t>(entry.file_size()));
      if (archive_write_header(output.get(), item.get()) != ARCHIVE_OK) throw ThemeError("Could not write theme export directory");
      std::ifstream stream(entry.path(), std::ios::binary);
      std::array<char, 65536> buffer{};
      while (stream) {
        stream.read(buffer.data(), buffer.size());
        const auto size = stream.gcount();
        if (size && archive_write_data(output.get(), buffer.data(), size) != size) throw ThemeError("Could not write theme export resource");
      }
      if (!stream.eof()) throw ThemeError("Could not read theme export resource");
    }
    if (archive_write_close(output.get()) != ARCHIVE_OK) throw ThemeError("Could not finish theme export");
    fs::rename(partial, destination);
  } catch (...) { archive_write_close(output.get()); std::error_code ignored; fs::remove(partial, ignored); throw; }
}

void ThemeLibrary::remove_theme(const std::string& id) {
  const auto theme = preview(id);
  if (auto selected = active(); selected && selected->id == id)
    throw ThemeError("Restore the original appearance or select another theme before removing this one");
  const auto trash = root_ / "themes" / (".removed-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::rename(theme.directory, trash);
  fs::remove_all(trash);
}

json ThemeLibrary::read_state() const {
  for (const auto& path : {root_ / "active.json", root_ / "active.json.bak"})
    if (fs::is_regular_file(path)) try { auto value = read_json(path); if (value.is_object()) return value; } catch (...) {}
  return json::object();
}
void ThemeLibrary::write_state(const json& value) const { atomic_json(root_ / "active.json", value); }

AppliedTheme ThemeLibrary::resolve(const std::string& id, ComponentOptions options) const {
  const auto theme = preview(id);
  AppliedTheme out;
  out.id = theme.id; out.name = theme.name; out.directory = theme.directory; out.components = options; out.warnings = theme.warnings;
  out.settings = theme.settings;
  out.shell = read_state().value("baseline", json::object());
  if (options.icons) out.icons = theme.icons;
  if (options.sounds) {
    out.sounds = theme.sounds;
    const std::map<std::string, std::vector<std::string>> roles{{"se_cursor", {"cursor", "cursor_up", "cursor_down"}},
        {"se_decide", {"enter"}}, {"se_cancel", {"back"}}, {"se_optionmenu", {"options"}}, {"se_system_ok", {"ok"}},
        {"cursor", {"cursor_up", "cursor_down"}}, {"enter", {"error"}}};
    for (const auto& [source, aliases] : roles) if (theme.sounds.contains(source))
      for (const auto& alias : aliases) out.sounds.try_emplace(alias, theme.sounds.at(source));
  }
  if (options.background && !theme.background.empty()) {
    const auto extension = lower(theme.background.extension().string());
    const std::set<std::string> videos{".mp4", ".webm", ".mkv", ".mov", ".avi", ".m4v", ".wmv", ".mpg", ".mpeg"};
    out.shell["background-type"] = videos.contains(extension) ? "video" : "image";
    out.shell["background-image"] = theme.background.string();
  }
  if (options.font && !theme.font.empty()) out.shell["font-path"] = theme.font.string();
  if (options.background && theme.settings.contains("background-type") && theme.settings["background-type"].is_string()) {
    const auto type = theme.settings["background-type"].get<std::string>();
    if (type == "original" || type == "wave" || type == "color") out.shell["background-type"] = type;
  }
  const std::map<std::string, std::string> color_keys = {{"backgroundColor", "background-color"}, {"waveColor", "wave-color"},
      {"iconColor", "icon-color"}, {"textColor", "text-color"}, {"clockColor", "clock-color"}, {"background-color", "background-color"},
      {"wave-color", "wave-color"}, {"icon-color", "icon-color"}, {"text-color", "text-color"}, {"theme-custom-colour", "theme-custom-colour"}};
  if (options.colors) for (const auto& [source, target] : color_keys)
    if (theme.settings.contains(source) && theme.settings[source].is_string()) out.shell[target] = theme.settings[source];
  if (options.colors) for (const auto* key : {"theme-colour-mode", "theme-day-night-mode", "clock-color", "text-color"})
    if (theme.settings.contains(key) && theme.settings[key].is_string()) out.shell[key] = theme.settings[key];
  if (options.colors && theme.settings.contains("backgroundColor") && theme.settings["backgroundColor"].is_string() &&
      theme.settings["backgroundColor"] == "auto" && theme.background.empty()) {
    out.shell["background-type"] = "original";
    out.shell["theme-colour-mode"] = "original";
    out.shell.erase("background-color");
  } else if (options.colors && theme.settings.contains("backgroundColor") && theme.settings["backgroundColor"].is_string() &&
             theme.settings["backgroundColor"].get_ref<const std::string&>().starts_with('#')) {
    out.shell["theme-custom-colour"] = theme.settings["backgroundColor"];
    out.shell["theme-colour-mode"] = "custom";
  }
  if (theme.settings.contains("wavesEnabled") && theme.settings["wavesEnabled"].is_boolean() && options.background)
    out.shell["theme-waves-enabled"] = theme.settings["wavesEnabled"];
  if (options.background && theme.settings.contains("waveParams") && theme.settings["waveParams"].is_object())
    out.shell["wave-parameters"] = theme.settings["waveParams"];
  if (options.background && theme.settings.contains("wave-parameters") && theme.settings["wave-parameters"].is_object())
    out.shell["wave-parameters"] = theme.settings["wave-parameters"];
  if (out.shell.contains("wave-parameters") && out.shell["wave-parameters"].is_object()) {
    json normalized = json::object();
    for (const auto& [key, value] : out.shell["wave-parameters"].items()) {
      const auto separator = key.find(':');
      if (separator != std::string::npos && value.is_number() && std::isfinite(value.get<double>()))
        normalized[key.substr(0, separator)][key.substr(separator + 1)] = value;
      else if (value.is_object()) for (const auto& [parameter, number] : value.items())
        if (number.is_number() && std::isfinite(number.get<double>())) normalized[key][parameter] = number;
    }
    out.shell["wave-parameters"] = std::move(normalized);
  }
  if (options.icons && theme.settings.contains("iconStyle") && theme.settings["iconStyle"].is_string())
    out.shell["icon-style"] = theme.settings["iconStyle"];
  if (options.icons && theme.settings.contains("icon-glass-refraction") && theme.settings["icon-glass-refraction"].is_boolean())
    out.shell["icon-glass-refraction"] = theme.settings["icon-glass-refraction"];
  if (options.font && theme.settings.contains("fontSize") && theme.settings["fontSize"].is_number_integer())
    out.shell["font-size"] = std::clamp(theme.settings["fontSize"].get<int>(), 50, 200);
  if (options.font && theme.settings.contains("font-size") && theme.settings["font-size"].is_number_integer())
    out.shell["font-size"] = std::clamp(theme.settings["font-size"].get<int>(), 50, 200);
  if (options.background || options.overlay) {
    out.animation = theme.animation;
    if (!options.background) out.animation.erase("frames");
    if (!options.overlay) out.animation.erase("layers");
    if (out.animation.value("kind", std::string{}) == "raf") {
      out.animation["background_enabled"] = options.background;
      out.animation["overlay_enabled"] = options.overlay;
    }
    if (out.animation.contains("frames")) for (auto& frame : out.animation["frames"])
      frame["file"] = resource(theme.directory, frame["file"], true).string();
    if (out.animation.contains("layers")) for (auto& layer : out.animation["layers"])
      layer["file"] = resource(theme.directory, layer["file"], true).string();
  }
  return out;
}

void ThemeLibrary::set_baseline(json shell) {
  if (!shell.is_object()) throw ThemeError("Baseline shell settings must be an object");
  auto state = read_state();
  if (!state.contains("baseline") || !state.contains("active")) { state["baseline"] = std::move(shell); write_state(state); }
}

AppliedTheme ThemeLibrary::apply(const std::string& id, ComponentOptions options) {
  auto out = resolve(id, options);
  auto state = read_state();
  state["active"] = {{"id", id}, {"components", options_json(options)}};
  write_state(state);
  return out;
}

AppliedTheme ThemeLibrary::revert() {
  auto state = read_state();
  AppliedTheme out;
  out.name = "Default"; out.reverted = true; out.shell = state.value("baseline", json::object());
  state.erase("active"); write_state(state);
  return out;
}

std::optional<AppliedTheme> ThemeLibrary::active() const {
  auto state = read_state();
  if (!state.contains("active") || !state["active"].is_object()) return std::nullopt;
  try { return resolve(state["active"].value("id", std::string{}), options_from(state["active"].value("components", json::object()))); }
  catch (const std::exception&) { return std::nullopt; }
}

void ThemeLibrary::save_preset(const std::string& name, const std::string& id, ComponentOptions options) {
  (void)preview(id);
  if (name.empty() || name.size() > 128) throw ThemeError("Enter a preset name");
  const auto path = root_ / "presets" / (slug(name) + ".json");
  if (fs::is_regular_file(path) && read_json(path).value("name", std::string{}) != name)
    throw ThemeError("This preset name conflicts with another saved name");
  atomic_json(path, {{"name", name}, {"id", id}, {"components", options_json(options)}});
}
std::vector<std::string> ThemeLibrary::presets() const {
  std::vector<std::string> out;
  for (const auto& entry : fs::directory_iterator(root_ / "presets"))
    if (entry.is_regular_file() && entry.path().extension() == ".json") try { out.push_back(read_json(entry.path()).at("name").get<std::string>()); } catch (...) {}
  std::ranges::sort(out);
  return out;
}
AppliedTheme ThemeLibrary::load_preset(const std::string& name) {
  auto preset = read_json(root_ / "presets" / (slug(name) + ".json"));
  return apply(preset.at("id").get<std::string>(), options_from(preset.value("components", json::object())));
}
void ThemeLibrary::delete_preset(const std::string& name) {
  const auto path = root_ / "presets" / (slug(name) + ".json");
  if (fs::is_symlink(path) || read_json(path).value("name", std::string{}) != name) throw ThemeError("Unknown theme preset");
  fs::remove(path);
  const auto backup = fs::path(path.string() + ".bak");
  if (fs::is_regular_file(backup) && !fs::is_symlink(backup)) fs::remove(backup);
}

AnimationFrame ThemeLibrary::sample_animation(const ThemeInfo& theme, double seconds) {
  AnimationFrame frame;
  frame.background = theme.background;
  frame.warnings = theme.warnings;
  seconds = std::isfinite(seconds) ? std::max(0.0, seconds) : 0;
  const auto& animation = theme.animation;
  if (animation.value("kind", std::string{}) == "raf") {
    auto animated = sample_raf_animation(animation, theme.directory, seconds);
    if (animated.background.empty()) animated.background = theme.background;
    animated.warnings.insert(animated.warnings.begin(), theme.warnings.begin(), theme.warnings.end());
    return animated;
  }
  if (animation.contains("frames") && !animation["frames"].empty()) {
    double total = 0;
    for (const auto& f : animation["frames"]) total += f.value("duration_ms", 1000.0);
    double position = std::fmod(seconds * 1000, total);
    for (const auto& f : animation["frames"]) {
      if (position < f.value("duration_ms", 1000.0)) { frame.background = resource(theme.directory, f["file"], true); break; }
      position -= f.value("duration_ms", 1000.0);
    }
  }
  if (animation.contains("layers")) for (const auto& layer : animation["layers"]) {
    AnimationLayer out;
    out.image = resource(theme.directory, layer["file"], true);
    const auto duration = layer.value("duration", 0.0);
    const double time = duration > 0 ? std::fmod(seconds, duration) : seconds;
    if ((layer.contains("show_start") && time < layer["show_start"].get<double>()) ||
        (layer.contains("show_end") && time >= layer["show_end"].get<double>())) continue;
    json values = layer;
    if (layer.contains("keys") && !layer["keys"].empty()) {
      const auto& keys = layer["keys"];
      const json* before = &keys.front(); const json* after = before;
      for (const auto& key : keys) { if (key.value("time", 0.0) <= time) before = &key; else { after = &key; break; } after = before; }
      const auto span = after->value("time", 0.0) - before->value("time", 0.0);
      const double mix = span > 0 ? std::clamp((time - before->value("time", 0.0)) / span, 0.0, 1.0) : 0;
      for (const auto* field : {"x", "y", "width", "height", "opacity"}) {
        const double base = layer.value(field, std::string_view(field) == "width" || std::string_view(field) == "height" || std::string_view(field) == "opacity" ? 1.0 : 0.0);
        values[field] = before->value(field, base) + (after->value(field, before->value(field, base)) - before->value(field, base)) * mix;
      }
    }
    out.x = values.value("x", 0.0F); out.y = values.value("y", 0.0F);
    out.width = values.value("width", 1.0F); out.height = values.value("height", 1.0F);
    out.opacity = std::clamp(values.value("opacity", 1.0F), 0.0F, 1.0F);
    out.additive = layer.value("additive", false);
    frame.layers.push_back(std::move(out));
  }
  return frame;
}

AnimationFrame ThemeLibrary::sample_animation(const AppliedTheme& applied, double seconds) {
  ThemeInfo theme;
  theme.directory = applied.directory;
  theme.animation = applied.animation;
  theme.warnings = applied.warnings;
  if (applied.shell.contains("background-image") && applied.shell["background-image"].is_string())
    theme.background = applied.shell["background-image"].get<std::string>();
  for (const auto* collection : {"frames", "layers"}) if (theme.animation.contains(collection))
    for (auto& item : theme.animation[collection]) {
      const fs::path path(item.at("file").get<std::string>());
      if (!contained(theme.directory, path)) throw ThemeError("Animation resource is outside its theme");
      item["file"] = path.lexically_relative(theme.directory).generic_string();
    }
  return sample_animation(theme, seconds);
}
} // namespace openxmb::themes
