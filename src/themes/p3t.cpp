#include "openxmb/themes/library.hpp"
#include "openxmb/themes/raf_runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <vector>
#include <zlib.h>

namespace openxmb::themes {
namespace {
using Bytes = std::vector<unsigned char>;
using View = std::span<const unsigned char>;
using json = nlohmann::json;
namespace fs = std::filesystem;

View slice(View data, std::size_t start, std::size_t size) {
  if (start > data.size() || size > data.size() - start) throw ThemeError("Truncated PS3 theme resource");
  return data.subspan(start, size);
}
std::uint32_t word(View data, std::size_t offset, bool big = true) {
  const auto b = slice(data, offset, 4);
  return big ? (std::uint32_t(b[0]) << 24) | (std::uint32_t(b[1]) << 16) | (std::uint32_t(b[2]) << 8) | b[3]
             : (std::uint32_t(b[3]) << 24) | (std::uint32_t(b[2]) << 16) | (std::uint32_t(b[1]) << 8) | b[0];
}
std::uint16_t half(View data, std::size_t offset, bool big = true) {
  auto b = slice(data, offset, 2);
  return big ? (b[0] << 8) | b[1] : (b[1] << 8) | b[0];
}
bool starts(View data, std::string_view prefix) { return data.size() >= prefix.size() && std::memcmp(data.data(), prefix.data(), prefix.size()) == 0; }
std::string string_at(View table, std::size_t offset) {
  if (offset >= table.size()) throw ThemeError("Invalid PS3 theme string offset");
  auto end = std::find(table.begin() + offset, table.end(), 0);
  if (end == table.end() || end - (table.begin() + offset) > 4096) throw ThemeError("Invalid PS3 theme string");
  return {reinterpret_cast<const char*>(table.data() + offset), static_cast<std::size_t>(end - (table.begin() + offset))};
}
bool safe_id(std::string_view id) {
  return !id.empty() && id.size() <= 128 && std::ranges::all_of(id, [](unsigned char c) { return (std::isalnum(c) && c < 128) || c == '_' || c == '-'; });
}

Bytes inflate(View data, std::uint64_t maximum) {
  z_stream stream{};
  if (inflateInit(&stream) != Z_OK) throw ThemeError("Could not initialize theme decompression");
  struct Cleanup { z_stream* stream; ~Cleanup() { inflateEnd(stream); } } cleanup{&stream};
  stream.next_in = const_cast<Bytef*>(data.data());
  stream.avail_in = static_cast<uInt>(data.size());
  Bytes out;
  std::array<unsigned char, 65536> buffer{};
  int status = Z_OK;
  while (status == Z_OK) {
    stream.next_out = buffer.data(); stream.avail_out = buffer.size();
    status = ::inflate(&stream, Z_NO_FLUSH);
    const auto size = buffer.size() - stream.avail_out;
    if (out.size() + size > maximum) throw ThemeError("PS3 theme decompression exceeds its resource budget");
    out.insert(out.end(), buffer.begin(), buffer.begin() + size);
  }
  if (status != Z_STREAM_END) throw ThemeError("Corrupt compressed PS3 theme resource");
  return out;
}

struct Attribute { json value; View file; bool binary = false; };
struct Element {
  std::string name;
  unsigned depth = 0;
  std::map<std::string, Attribute> attributes;
  json get(const std::string& key, json fallback = {}) const { auto i = attributes.find(key); return i == attributes.end() ? fallback : i->second.value; }
  std::string text(const std::string& key) const { auto v = get(key); return v.is_string() ? v.get<std::string>() : std::string{}; }
};

std::vector<Element> parse(View data, const ImportLimits& limits) {
  if (!starts(data, "P3TF") && !starts(data, "RAFO")) throw ThemeError("Not a PS3 theme resource tree");
  const auto tree = slice(data, word(data, 8), word(data, 12));
  const auto ids = slice(data, word(data, 16), word(data, 20));
  const auto strings = slice(data, word(data, 24), word(data, 28));
  const auto integers = slice(data, word(data, 32), word(data, 36));
  const auto floats = slice(data, word(data, 40), word(data, 44));
  const auto files = slice(data, word(data, 48), word(data, 52));
  std::vector<std::pair<std::uint32_t, unsigned>> pending{{0, 0}};
  std::set<std::uint32_t> visited;
  std::vector<Element> out;
  std::size_t total_attributes = 0, total_array_values = 0, text_bytes = 0;
  while (!pending.empty()) {
    const auto [offset, depth] = pending.back(); pending.pop_back();
    if (depth > 128 || !visited.insert(offset).second || out.size() >= limits.entries) throw ThemeError("PS3 theme tree is cyclic or too large");
    slice(tree, offset, 28);
    Element element{string_at(strings, word(tree, offset)), depth, {}};
    const auto count = word(tree, offset + 4);
    total_attributes += count;
    if (count > 512 || total_attributes > 65536) throw ThemeError("Too many PS3 theme attributes");
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto at = static_cast<std::size_t>(offset) + 28 + 16ULL * i;
      const auto name = string_at(strings, word(tree, at));
      const auto kind = word(tree, at + 4), v1 = word(tree, at + 8), v2 = word(tree, at + 12);
      Attribute a;
      switch (kind) {
        case 1: a.value = static_cast<std::int32_t>(v1); break;
        case 2: { auto number = std::bit_cast<float>(v1); if (!std::isfinite(number)) throw ThemeError("Nonfinite theme number"); a.value = number; break; }
        case 3: a.value = string_at(strings, v1); break;
        case 4: case 5: {
          total_array_values += v2;
          if (v2 > 65536 || total_array_values > 1000000) throw ThemeError("Theme attribute array exceeds its budget");
          a.value = json::array();
          for (std::uint32_t j = 0; j < v2; ++j) {
            const auto at = (static_cast<std::uint64_t>(v1) + j) * 4;
            if (kind == 4) a.value.push_back(static_cast<std::int32_t>(word(integers, at)));
            else { const auto n = std::bit_cast<float>(word(floats, at)); if (!std::isfinite(n)) throw ThemeError("Nonfinite theme number"); a.value.push_back(n); }
          }
          break;
        }
        case 6: a.file = slice(files, v1, v2); a.binary = true; break;
        case 7: case 8: a.value = string_at(ids, static_cast<std::uint64_t>(v1) + 4); break;
        default: continue;
      }
      if (a.value.is_string()) text_bytes += a.value.get_ref<const std::string&>().size();
      if (text_bytes > 16 * 1024 * 1024) throw ThemeError("PS3 theme strings exceed their resource budget");
      element.attributes.emplace(name, std::move(a));
    }
    out.push_back(std::move(element));
    const auto next = word(tree, offset + 16), child = word(tree, offset + 20);
    if (next != 0xffffffffU) pending.emplace_back(next, depth);
    if (child != 0xffffffffU) pending.emplace_back(child, depth + 1);
  }
  return out;
}

Bytes payload(const Element& element, const std::string& key, bool compressed, const ImportLimits& limits) {
  auto found = element.attributes.find(key);
  if (found == element.attributes.end() || !found->second.binary) return {};
  const auto data = found->second.file;
  if (data.size() > limits.file_bytes) throw ThemeError("PS3 theme resource exceeds its size limit");
  // Themes in the wild contain both compressed and already-native payloads.
  if (compressed && data.size() >= 2 && data[0] == 0x78 && ((data[0] << 8) + data[1]) % 31 == 0)
    return inflate(data, limits.file_bytes);
  return Bytes(data.begin(), data.end());
}

void save(const fs::path& file, View data) {
  fs::create_directories(file.parent_path());
  std::ofstream out(file, std::ios::binary);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!out) throw ThemeError("Could not save imported PS3 theme resource");
}
void append_word(Bytes& out, std::uint32_t value, bool big = true) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<unsigned char>(value >> (big ? 24 - 8 * i : 8 * i)));
}

Bytes png(unsigned width, unsigned height, View rgba) {
  Bytes out{137, 80, 78, 71, 13, 10, 26, 10};
  const auto chunk = [&](std::string_view type, View bytes) {
    append_word(out, bytes.size());
    auto crc = crc32(0, reinterpret_cast<const Bytef*>(type.data()), type.size());
    crc = crc32(crc, bytes.data(), bytes.size());
    out.insert(out.end(), type.begin(), type.end()); out.insert(out.end(), bytes.begin(), bytes.end()); append_word(out, crc);
  };
  Bytes header;
  append_word(header, width); append_word(header, height);
  header.insert(header.end(), {8, 6, 0, 0, 0}); chunk("IHDR", header);
  Bytes filtered;
  filtered.reserve(rgba.size() + height);
  for (unsigned y = 0; y < height; ++y) {
    filtered.push_back(0);
    const auto row = slice(rgba, static_cast<std::size_t>(y) * width * 4, width * 4);
    filtered.insert(filtered.end(), row.begin(), row.end());
  }
  uLongf size = compressBound(filtered.size());
  Bytes compressed(size);
  if (compress2(compressed.data(), &size, filtered.data(), filtered.size(), 6) != Z_OK) throw ThemeError("Could not encode PS3 theme image");
  compressed.resize(size); chunk("IDAT", compressed); chunk("IEND", {});
  return out;
}

Bytes expand(unsigned format, View pixels, std::size_t count, bool big) {
  if (format > 3 || count > 16ULL * 1024 * 1024) throw ThemeError("Unsupported GIM pixel format");
  Bytes result(count * 4, 0);
  const auto bpp = format == 3 ? 4 : 2;
  slice(pixels, 0, count * bpp);
  for (std::size_t i = 0; i < count; ++i) {
    auto* out = result.data() + i * 4;
    if (format == 3) { std::memcpy(out, pixels.data() + i * 4, 4); continue; }
    const auto v = half(pixels, i * 2, big);
    if (format == 0) { out[0] = (v & 31) * 255 / 31; out[1] = ((v >> 5) & 63) * 255 / 63; out[2] = ((v >> 11) & 31) * 255 / 31; out[3] = 255; }
    else if (format == 1) { out[0] = (v & 31) * 255 / 31; out[1] = ((v >> 5) & 31) * 255 / 31; out[2] = ((v >> 10) & 31) * 255 / 31; out[3] = v >> 15 ? 255 : 0; }
    else { out[0] = (v & 15) * 17; out[1] = ((v >> 4) & 15) * 17; out[2] = ((v >> 8) & 15) * 17; out[3] = (v >> 12) * 17; }
  }
  return result;
}

Bytes gim_png(View data, const ImportLimits& limits) {
  const bool big = starts(data, ".GIM");
  if (!big && !starts(data, "MIG.")) {
    if (starts(data, "\x89PNG")) return Bytes(data.begin(), data.end());
    throw ThemeError("Unsupported PS3 theme icon format");
  }
  const auto u16 = [&](std::size_t offset) { return half(data, offset, big); };
  const auto u32 = [&](std::size_t offset) { return word(data, offset, big); };
  std::optional<std::size_t> image, palette;
  std::vector<std::pair<std::size_t, std::size_t>> pending{{16, data.size()}};
  unsigned blocks = 0;
  while (!pending.empty()) {
    auto [offset, end] = pending.back(); pending.pop_back();
    while (offset + 16 <= end) {
      if (++blocks > 10000) throw ThemeError("Too many GIM blocks");
      const auto kind = u16(offset);
      const auto size = u32(offset + 4), start = u32(offset + 12);
      if (size < 16 || size > end - offset || start > size) throw ThemeError("Invalid GIM block bounds");
      if (kind == 2 || kind == 3) pending.emplace_back(offset + start, offset + size);
      else if (kind == 4 && !image) image = offset + start;
      else if (kind == 5 && !palette) palette = offset + start;
      offset += size;
    }
  }
  if (!image) throw ThemeError("GIM has no image");
  const auto header = [&](std::size_t at) {
    return std::array<std::size_t, 6>{u16(at + 4), u16(at + 6), u16(at + 8), u16(at + 10), at + u32(at + 0x1c), at + u32(at + 0x20)};
  };
  const auto [format, order, width, height, start, stop] = header(*image);
  if (!width || !height || width > limits.image_dimension || height > limits.image_dimension || stop < start || format > 5)
    throw ThemeError("GIM dimensions or format exceed supported limits");
  auto pixels_view = slice(data, start, stop - start);
  Bytes pixels(pixels_view.begin(), pixels_view.end());
  const auto pitch = pixels.size() / height;
  const auto required_pitch = format == 3 ? width * 4 : format < 3 ? width * 2 : format == 4 ? (width + 1) / 2 : width;
  if (pitch < required_pitch) throw ThemeError("Truncated GIM pixel rows");
  if (order == 1) {
    Bytes linear(pixels.size(), 0);
    std::size_t source = 0;
    for (std::size_t y = 0; y < height; y += 8) for (std::size_t x = 0; x < pitch; x += 16)
      for (std::size_t row = y; row < std::min(y + 8, height); ++row) {
        const auto destination = row * pitch + x;
        const auto n = std::min({std::size_t(16), linear.size() - destination, pixels.size() > source ? pixels.size() - source : 0});
        if (n) std::memcpy(linear.data() + destination, pixels.data() + source, n);
        source += 16;
      }
    pixels = std::move(linear);
  }
  Bytes rgba;
  rgba.reserve(width * height * 4);
  Bytes colors;
  if (format == 4 || format == 5) {
    if (!palette) throw ThemeError("GIM palette is missing");
    const auto [pf, po, pw, ph, ps, pe] = header(*palette);
    if (pe < ps || pw > 256) throw ThemeError("Invalid GIM palette");
    colors = expand(pf, slice(data, ps, pe - ps), pw, big);
  }
  for (std::size_t y = 0; y < height; ++y) {
    const auto row = slice(pixels, y * pitch, pitch);
    if (format < 4) {
      const auto expanded = expand(format, row, width, big);
      rgba.insert(rgba.end(), expanded.begin(), expanded.end());
    } else for (std::size_t x = 0; x < width; ++x) {
      const auto index = format == 4 ? ((row[x / 2] >> (4 * (x % 2))) & 15) : row[x];
      auto color = slice(colors, index * 4, 4);
      rgba.insert(rgba.end(), color.begin(), color.end());
    }
  }
  return png(width, height, rgba);
}

Bytes vag_wav(View data, const ImportLimits& limits, View right = {}) {
  const bool header = starts(data, "VAGp");
  const auto rate = header ? word(data, 16) : 44100;
  if (rate < 8000 || rate > 192000) throw ThemeError("Invalid PS3 sound sample rate");
  const std::size_t start = header ? 48 : 0;
  const std::array<std::array<double, 2>, 5> coefficients{{{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}}};
  Bytes samples;
  double previous = 0, previous2 = 0;
  for (std::size_t p = start; p + 16 <= data.size(); p += 16) {
    const auto filter = data[p] >> 4, shift = data[p] & 15;
    if (filter > 4 || shift > 12) throw ThemeError("Invalid PS3 sound ADPCM block");
    for (unsigned i = 0; i < 28; ++i) {
      int nibble = (data[p + 2 + i / 2] >> (4 * (i % 2))) & 15;
      if (nibble >= 8) nibble -= 16;
      const auto value = std::clamp(std::lround((nibble * 4096) / double(1 << shift) + (previous * coefficients[filter][0] + previous2 * coefficients[filter][1]) / 64), -32768L, 32767L);
      previous2 = previous; previous = value;
      samples.push_back(value & 255); samples.push_back((value >> 8) & 255);
      if (samples.size() > limits.file_bytes) throw ThemeError("PS3 sound exceeds its decoded size limit");
    }
    if (data[p + 1] == 7) break;
  }
  unsigned channels = 1;
  if (!right.empty()) {
    const auto other = vag_wav(right, limits);
    if (word(other, 24, false) != rate) throw ThemeError("Stereo PS3 sounds have mismatched sample rates");
    const auto right_samples = slice(other, 44, other.size() - 44);
    const auto count = std::max(samples.size(), right_samples.size()) / 2;
    if (count * 4 > limits.file_bytes) throw ThemeError("Stereo PS3 sound exceeds its decoded size limit");
    Bytes stereo(count * 4, 0);
    for (std::size_t i = 0; i < count; ++i) {
      if (i * 2 + 1 < samples.size()) { stereo[i * 4] = samples[i * 2]; stereo[i * 4 + 1] = samples[i * 2 + 1]; }
      if (i * 2 + 1 < right_samples.size()) { stereo[i * 4 + 2] = right_samples[i * 2]; stereo[i * 4 + 3] = right_samples[i * 2 + 1]; }
    }
    samples = std::move(stereo); channels = 2;
  }
  Bytes out{'R','I','F','F'}; append_word(out, samples.size() + 36, false);
  out.insert(out.end(), {'W','A','V','E','f','m','t',' '}); append_word(out, 16, false);
  out.insert(out.end(), {1,0,static_cast<unsigned char>(channels),0}); append_word(out, rate, false); append_word(out, rate * 2 * channels, false);
  out.insert(out.end(), {static_cast<unsigned char>(2 * channels),0,16,0,'d','a','t','a'}); append_word(out, samples.size(), false);
  out.insert(out.end(), samples.begin(), samples.end()); return out;
}

json import_raf(View blob, const fs::path& output, const ImportLimits& limits, std::vector<std::string>& warnings) {
  Bytes decompressed;
  if (starts(blob, "_RAF")) { decompressed = inflate(slice(blob, 8, blob.size() - 8), limits.file_bytes); blob = decompressed; }
  const auto tree = parse(blob, limits);
  RafDocument document;
  std::uint64_t binary_bytes = 0;
  for (const auto& element : tree) {
    json attributes = json::object();
    for (const auto& [key, attribute] : element.attributes)
      if (!attribute.binary) attributes[key] = attribute.value;
    document.elements.push_back({{"name", element.name}, {"depth", element.depth}, {"attributes", std::move(attributes)}});
    if (element.name == "file") {
      auto bytes = payload(element, "src", false, limits);
      binary_bytes += bytes.size();
      if (binary_bytes > limits.expanded_bytes) throw ThemeError("RAF files exceed their resource budget");
      document.files.push_back(std::move(bytes));
    }
  }
  return compile_raf_animation(document, output, limits, warnings);
}
} // namespace

nlohmann::json import_p3t(const fs::path& source, const fs::path& output, const ImportLimits& limits) {
  if (!fs::is_regular_file(source) || fs::file_size(source) > limits.archive_bytes) throw ThemeError("PS3 theme exceeds its archive size limit");
  std::ifstream stream(source, std::ios::binary);
  Bytes bytes(static_cast<std::size_t>(fs::file_size(source)));
  stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  if (!stream) throw ThemeError("Could not read PS3 theme");
  if (!starts(bytes, "P3TF")) throw ThemeError("Not a .p3t theme");
  const auto tree = parse(bytes, limits);
  json manifest = {{"format", "openxmb-theme"}, {"version", 1}, {"name", source.stem().string()},
      {"icons", json::object()}, {"sounds", json::object()}, {"settings", json::object()}};
  std::vector<std::string> warnings;
  std::uint64_t expanded = 0;
  const auto write = [&](const std::string& name, View data) {
    expanded += data.size();
    if (expanded > limits.expanded_bytes || data.size() > limits.file_bytes) throw ThemeError("PS3 theme exceeds its expanded resource budget");
    save(output / name, data);
  };
  for (const auto& element : tree) {
    if (element.name == "info") {
      for (const auto* key : {"name", "author", "comment"}) if (!element.text(key).empty()) manifest[key] = element.text(key);
      for (const auto& [key, file] : {std::pair{"preview", "preview.png"}, std::pair{"icon", "thumbnail.png"}}) {
        auto raw = payload(element, key, true, limits);
        if (raw.empty()) continue;
        try { const auto image = gim_png(raw, limits); write(file, image); if (key == std::string_view("preview")) manifest["preview"] = file; }
        catch (const ThemeError& error) { warnings.push_back(std::string(key) + ": " + error.what()); }
      }
    } else if (element.name == "icon" && safe_id(element.text("id"))) {
      auto raw = payload(element, "src", true, limits);
      if (raw.empty()) continue;
      try { const auto image = gim_png(raw, limits); const auto file = "icons/" + element.text("id") + ".png"; write(file, image); manifest["icons"][element.text("id") + ".png"] = file; }
      catch (const ThemeError& error) { warnings.push_back(element.text("id") + ": " + error.what()); }
    } else if (element.name == "bgimage") {
      if (!manifest.contains("background")) {
        auto raw = payload(element, "hd", true, limits); if (raw.empty()) raw = payload(element, "sd", true, limits);
        if (!raw.empty()) {
          const auto file = starts(raw, "\x89PNG") ? "background.png" : "background.jpg";
          write(file, raw); manifest["background"] = file;
        }
      }
      const auto animation = payload(element, "anim", true, limits);
      if (!animation.empty()) try { manifest["animation"] = import_raf(animation, output, limits, warnings); }
      catch (const ThemeError& error) { warnings.push_back(std::string("Animated background: ") + error.what()); }
    } else if (element.name == "se" && safe_id(element.text("id"))) {
      auto raw = payload(element, "src", true, limits);
      Bytes right;
      if (raw.empty()) { raw = payload(element, "left", true, limits); right = payload(element, "right", true, limits); }
      if (raw.empty()) continue;
      try {
        const auto wav = starts(raw, "RIFF") ? raw : vag_wav(raw, limits, right);
        const auto file = "sounds/" + element.text("id") + ".wav"; write(file, wav); manifest["sounds"][element.text("id")] = file;
      } catch (const ThemeError& error) { warnings.push_back(element.text("id") + ": " + error.what()); }
    } else if (element.name == "color") {
      const auto selection = element.get("selection");
      if (selection.is_number_integer()) manifest["settings"]["ps3-color-selection"] = selection;
    }
  }
  if (manifest["icons"].empty() && manifest["sounds"].empty() && !manifest.contains("background") && !manifest.contains("animation"))
    throw ThemeError("This .p3t contains no supported theme resources");
  manifest["warnings"] = warnings;
  return manifest;
}
} // namespace openxmb::themes
