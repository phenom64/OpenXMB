#include "openxmb/desktop/trophy_sound.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace openxmb::desktop {
namespace {
using Status = TrophySound::CueStatus;
using Result = TrophySound::CueResult;

constexpr std::size_t kWavHeader = 44;

std::uint32_t le16(const std::uint8_t *p) { return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8; }
std::uint32_t le32(const std::uint8_t *p) { return le16(p) | le16(p + 2) << 16; }
void put16(std::vector<std::uint8_t> &out, std::uint32_t v) { out.push_back(static_cast<std::uint8_t>(v)); out.push_back(static_cast<std::uint8_t>(v >> 8)); }
void put32(std::vector<std::uint8_t> &out, std::uint32_t v) { put16(out, v & 0xffffu); put16(out, v >> 16); }
void put_tag(std::vector<std::uint8_t> &out, const char *tag) { out.insert(out.end(), tag, tag + 4); }

bool valid_utf8(std::string_view s) {
  for (std::size_t i = 0; i < s.size();) {
    const auto c = static_cast<std::uint8_t>(s[i]);
    std::size_t n = 0;
    std::uint32_t cp = 0;
    if (c < 0x80) { ++i; continue; }
    if (c >= 0xc2 && c <= 0xdf) { n = 1; cp = c & 0x1fu; }
    else if (c >= 0xe0 && c <= 0xef) { n = 2; cp = c & 0x0fu; }
    else if (c >= 0xf0 && c <= 0xf4) { n = 3; cp = c & 0x07u; }
    else return false;
    if (i + n >= s.size()) return false;
    for (std::size_t k = 1; k <= n; ++k) {
      const auto cc = static_cast<std::uint8_t>(s[i + k]);
      if ((cc & 0xc0) != 0x80) return false;
      cp = cp << 6 | (cc & 0x3fu);
    }
    if ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    i += n + 1;
  }
  return true;
}

// Absolute, local, UTF-8. Returns false for anything else without touching disk.
bool local_path(const std::string &text, fs::path &out) {
  if (text.empty() || text.size() > 4096 || text.find('\0') != std::string::npos || !valid_utf8(text)) return false;
#ifdef _WIN32
  // UNC, \\?\ and \\.\ device namespaces and NTFS alternate streams are not plain local files.
  const auto is_sep = [](char c) { return c == '\\' || c == '/'; };
  if (text.size() >= 2 && is_sep(text[0]) && is_sep(text[1])) return false;
  if (text.find(':', 2) != std::string::npos) return false;
#endif
  const std::u8string wide(reinterpret_cast<const char8_t *>(text.data()), text.size());
  fs::path path(wide);
  if (!path.is_absolute() || !path.has_filename()) return false;
  out = std::move(path);
  return true;
}

struct Parsed {
  Status status = Status::malformed;
  std::uint32_t rate = 0, channels = 0, bits = 0, duration_ms = 0;
  std::vector<std::uint8_t> canonical; // minimal 44-byte-header PCM WAV, exactly what SDL gets
};

// Validates every length against the bytes actually present; nothing is trusted.
Parsed parse_wav(const std::vector<std::uint8_t> &file) {
  Parsed r;
  const std::uint8_t *d = file.data();
  const std::size_t size = file.size();
  if (size < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) { r.status = Status::not_wav; return r; }
  const std::uint64_t extent = std::uint64_t{le32(d + 4)} + 8;
  if (extent < 12 || extent > size) { r.status = Status::malformed; return r; } // truncated or lying size
  const std::size_t end = static_cast<std::size_t>(extent);

  bool have_fmt = false, have_data = false;
  std::uint32_t tag = 0, align = 0;
  std::size_t data_at = 0, data_len = 0;
  std::size_t at = 12;
  while (at < end) {
    if (end - at < 8) { r.status = Status::malformed; return r; }
    const std::uint8_t *h = d + at;
    const std::uint64_t len = le32(h + 4);
    const std::size_t body = at + 8;
    if (len > end - body) { r.status = Status::malformed; return r; }
    if (std::memcmp(h, "fmt ", 4) == 0) {
      if (have_fmt || len < 16) { r.status = Status::malformed; return r; }
      have_fmt = true;
      tag = le16(d + body);
      r.channels = le16(d + body + 2);
      r.rate = le32(d + body + 4);
      align = le16(d + body + 12);
      r.bits = le16(d + body + 14);
    } else if (std::memcmp(h, "data", 4) == 0) {
      if (!have_fmt || have_data) { r.status = Status::malformed; return r; }
      have_data = true;
      data_at = body;
      data_len = static_cast<std::size_t>(len);
    }
    at = body + static_cast<std::size_t>(len) + (len & 1u); // RIFF pads odd chunks
  }
  if (!have_fmt || !have_data) { r.status = Status::malformed; return r; }

  if (tag != 1 || (r.bits != 8 && r.bits != 16) || (r.channels != 1 && r.channels != 2) ||
      r.rate < TrophySound::min_sample_rate || r.rate > TrophySound::max_sample_rate) {
    r.status = Status::unsupported_format;
    return r;
  }
  if (align != r.channels * r.bits / 8 || data_len == 0 || data_len % align != 0) { r.status = Status::malformed; return r; }
  const std::uint64_t frames = data_len / align;
  r.duration_ms = static_cast<std::uint32_t>(frames * 1000 / r.rate);
  if (frames * 1000 > std::uint64_t{TrophySound::max_duration_ms} * r.rate || data_len > TrophySound::max_file_bytes) { r.status = Status::too_long; return r; }

  r.canonical.reserve(kWavHeader + data_len);
  put_tag(r.canonical, "RIFF"); put32(r.canonical, static_cast<std::uint32_t>(36 + data_len)); put_tag(r.canonical, "WAVE");
  put_tag(r.canonical, "fmt "); put32(r.canonical, 16); put16(r.canonical, 1); put16(r.canonical, r.channels);
  put32(r.canonical, r.rate); put32(r.canonical, r.rate * align); put16(r.canonical, align); put16(r.canonical, r.bits);
  put_tag(r.canonical, "data"); put32(r.canonical, static_cast<std::uint32_t>(data_len));
  r.canonical.insert(r.canonical.end(), d + data_at, d + data_at + data_len);
  r.status = Status::ready;
  return r;
}

// Reads the whole bounded file. status is ready on success.
Status read_file(const std::string &text, std::vector<std::uint8_t> &bytes) {
  fs::path path;
  if (text.empty()) return Status::path_empty;
  if (!local_path(text, path)) return Status::path_invalid;
  std::error_code ec;
  const auto type = fs::status(path, ec).type();
  if (type == fs::file_type::not_found) return Status::not_found;
  if (ec) return Status::unreadable;
  if (type != fs::file_type::regular) return Status::not_regular_file;
  const auto size = fs::file_size(path, ec);
  if (ec) return Status::unreadable;
  if (size > TrophySound::max_file_bytes) return Status::too_large;
  std::ifstream in(path, std::ios::binary); // read-only
  if (!in) return Status::unreadable;
  bytes.resize(static_cast<std::size_t>(size));
  in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (static_cast<std::uintmax_t>(in.gcount()) != size) return Status::unreadable;
  if (in.peek() != std::ifstream::traits_type::eof()) return Status::too_large; // grew while reading
  return Status::ready;
}

struct Loaded { Parsed parsed; Result info; };

Loaded load(const std::string &path) {
  Loaded out;
  try {
    std::vector<std::uint8_t> bytes;
    out.info.status = read_file(path, bytes);
    if (out.info.status != Status::ready) return out;
    out.info.file_bytes = bytes.size();
    out.parsed = parse_wav(bytes);
    out.info.status = out.parsed.status;
    if (out.parsed.rate != 0 && (out.parsed.status == Status::ready || out.parsed.status == Status::too_long)) {
      out.info.sample_rate = out.parsed.rate; out.info.channels = out.parsed.channels;
      out.info.bits_per_sample = out.parsed.bits; out.info.duration_ms = out.parsed.duration_ms;
    }
  } catch (...) {
    out = {};
    out.info.status = Status::unreadable;
  }
  return out;
}

struct Spec {
  int frequency = 0, channels = 0;
  Uint16 format = 0;
  bool operator==(const Spec &) const = default;
};
bool query(Spec &spec) { return Mix_QuerySpec(&spec.frequency, &spec.format, &spec.channels) > 0; }
} // namespace

struct TrophySound::Impl {
  bool enabled = false;
  int master = 100, effects = 100, volume = MIX_MAX_VOLUME;
  std::uint64_t last_serial = 0;
  std::vector<std::uint8_t> canonical; // validated bytes kept so a reopened device needs no disk read
  CueResult cue;
  Mix_Chunk *chunk = nullptr;
  Spec chunk_spec;
  int channel = -1;

  // SDL_mixer reuses channels, and a finished channel still reports its last chunk, so a stored
  // index proves nothing: require the chunk to be ours AND still playing. After Mix_CloseAudio the
  // library keeps its channel count but frees the channel array, so per-channel calls on a closed
  // mixer would dereference null: every one is gated on the mixer being open.
  bool owns_channel() const noexcept {
    return chunk && channel >= 0 && Mix_QuerySpec(nullptr, nullptr, nullptr) > 0 &&
           Mix_GetChunk(channel) == chunk && Mix_Playing(channel) != 0;
  }
  void halt_own() noexcept {
    if (owns_channel()) Mix_HaltChannel(channel);
    channel = -1;
  }
  void release_chunk() noexcept {
    halt_own();
    if (chunk) Mix_FreeChunk(chunk);
    chunk = nullptr;
    cue.decoded = false;
  }
  bool decode() noexcept {
    Spec spec;
    if (canonical.empty() || !query(spec)) return false;
    SDL_RWops *rw = SDL_RWFromConstMem(canonical.data(), static_cast<int>(canonical.size()));
    if (!rw) return false;
    Mix_Chunk *loaded = Mix_LoadWAV_RW(rw, 1);
    if (!loaded) return false;
    if (loaded->alen == 0) { Mix_FreeChunk(loaded); return false; }
    chunk = loaded;
    chunk_spec = spec;
    Mix_VolumeChunk(chunk, volume);
    cue.decoded = true;
    return true;
  }
  // True when a chunk matching the current device exists.
  bool ensure_chunk() noexcept {
    Spec spec;
    if (!query(spec)) return false;
    if (chunk && chunk_spec == spec) return true;
    release_chunk(); // device format changed behind our back
    return decode();
  }
  void drop_cue() noexcept {
    release_chunk();
    canonical.clear();
    canonical.shrink_to_fit();
    cue = {};
  }
};

TrophySound::TrophySound() : impl_(std::make_unique<Impl>()) {}
TrophySound::~TrophySound() { impl_->release_chunk(); }

TrophySound::CueResult TrophySound::inspect(const std::string &utf8_path) { return load(utf8_path).info; }

TrophySound::CueResult TrophySound::configure(const Settings &settings) {
  Impl &s = *impl_;
  s.drop_cue();
  s.enabled = settings.enabled;
  set_volume(settings.master_percent, settings.effects_percent);
  if (!settings.enabled) return s.cue;
  auto loaded = load(settings.path);
  s.cue = loaded.info;
  s.cue.decoded = false;
  if (!s.cue.ok()) return s.cue;
  s.canonical = std::move(loaded.parsed.canonical);
  Spec spec;
  if (query(spec) && !s.decode()) {
    s.drop_cue();
    s.cue.status = CueStatus::decode_failed;
  }
  return s.cue;
}

void TrophySound::set_volume(int master_percent, int effects_percent) noexcept {
  Impl &s = *impl_;
  s.master = std::clamp(master_percent, 0, 100);
  s.effects = std::clamp(effects_percent, 0, 100);
  s.volume = (s.master * s.effects * MIX_MAX_VOLUME + 5000) / 10000;
  if (s.chunk) Mix_VolumeChunk(s.chunk, s.volume); // this chunk only; live while playing
  if (s.volume == 0) s.halt_own();
}

TrophySound::PlayStatus TrophySound::play_once(std::uint64_t notice_serial) {
  Impl &s = *impl_;
  if (notice_serial == 0) return PlayStatus::invalid_serial;
  if (notice_serial == s.last_serial) return PlayStatus::repeated;
  if (notice_serial < s.last_serial) return PlayStatus::stale;
  s.last_serial = notice_serial;
  if (!s.enabled) return PlayStatus::off;
  if (s.canonical.empty()) return PlayStatus::unavailable;
  if (s.volume == 0) { s.halt_own(); return PlayStatus::muted; }
  Spec spec;
  if (!query(spec)) return PlayStatus::device_closed;
  s.halt_own();
  if (!s.ensure_chunk()) return PlayStatus::play_failed;
  const int channel = Mix_PlayChannel(-1, s.chunk, 0);
  if (channel < 0) {
    // Mix_PlayChannel reports "no free channel" and genuine errors alike as -1.
    return std::strstr(Mix_GetError(), "No free channels") ? PlayStatus::no_channel : PlayStatus::play_failed;
  }
  s.channel = channel;
  return PlayStatus::played;
}

void TrophySound::stop() noexcept { impl_->halt_own(); }

void TrophySound::device_will_close() noexcept { impl_->release_chunk(); }

bool TrophySound::prepare() noexcept {
  Impl &s = *impl_;
  if (!s.enabled || s.canonical.empty()) return false;
  return s.ensure_chunk();
}

bool TrophySound::active() const noexcept { return impl_->owns_channel(); }
int TrophySound::channel() const noexcept { return impl_->channel; }
std::uint64_t TrophySound::last_serial() const noexcept { return impl_->last_serial; }
int TrophySound::volume() const noexcept { return impl_->volume; }
const TrophySound::CueResult &TrophySound::cue() const noexcept { return impl_->cue; }

const char *TrophySound::describe(CueStatus status) noexcept {
  switch (status) {
    case CueStatus::off: return "Trophy sound is off.";
    case CueStatus::ready: return "Trophy sound is ready.";
    case CueStatus::path_empty: return "Choose a trophy sound WAV file.";
    case CueStatus::path_invalid: return "Use the full local path of a WAV file (network and device paths are not supported).";
    case CueStatus::not_found: return "The trophy sound file was not found.";
    case CueStatus::not_regular_file: return "The trophy sound path is not a file.";
    case CueStatus::unreadable: return "The trophy sound file could not be read.";
    case CueStatus::too_large: return "The trophy sound file is larger than 2 MiB.";
    case CueStatus::not_wav: return "The trophy sound must be a WAV file.";
    case CueStatus::unsupported_format: return "The trophy sound must be uncompressed 8 or 16-bit mono or stereo PCM, 8 to 96 kHz.";
    case CueStatus::malformed: return "The trophy sound WAV file is damaged or incomplete.";
    case CueStatus::too_long: return "The trophy sound is longer than 5 seconds.";
    case CueStatus::decode_failed: return "The audio system could not load the trophy sound.";
  }
  return "";
}

const char *TrophySound::describe(PlayStatus status) noexcept {
  switch (status) {
    case PlayStatus::played: return "Trophy sound played.";
    case PlayStatus::repeated: return "Trophy sound already handled for this notice.";
    case PlayStatus::stale: return "Trophy sound ignored for an older notice.";
    case PlayStatus::invalid_serial: return "Trophy sound ignored: invalid notice.";
    case PlayStatus::off: return "Trophy sound is off.";
    case PlayStatus::unavailable: return "Trophy sound unavailable: no valid file is configured.";
    case PlayStatus::muted: return "Trophy sound is muted (master or effects volume is 0).";
    case PlayStatus::device_closed: return "Trophy sound skipped: audio output is not open.";
    case PlayStatus::no_channel: return "Trophy sound skipped: all audio channels are busy.";
    case PlayStatus::play_failed: return "Trophy sound could not be played.";
  }
  return "";
}
} // namespace openxmb::desktop
