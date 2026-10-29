// Exercises TrophySound against a real SDL_mixer on the dummy audio driver with
// private generated PCM fixtures. No real audio output, no shipped sound.
#define SDL_MAIN_HANDLED
#include "openxmb/desktop/trophy_sound.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using openxmb::desktop::TrophySound;
using Cue = TrophySound::CueStatus;
using Play = TrophySound::PlayStatus;
using Bytes = std::vector<std::uint8_t>;

namespace {
#define REQUIRE(cond) do { if (!(cond)) throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " #cond); } while (0)

// ---- independent WAV fixture writer (does not share code with the parser under test) ----
void le(Bytes &b, std::uint64_t v, int n) { for (int i = 0; i < n; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
void cat(Bytes &a, const Bytes &b) { a.insert(a.end(), b.begin(), b.end()); }
Bytes tag(const char *t) { return Bytes(t, t + 4); }

Bytes fmt_body(unsigned format, unsigned ch, unsigned rate, unsigned bits, int align = -1, int byte_rate = -1) {
  Bytes b;
  const unsigned a = align >= 0 ? static_cast<unsigned>(align) : ch * bits / 8;
  le(b, format, 2); le(b, ch, 2); le(b, rate, 4);
  le(b, byte_rate >= 0 ? static_cast<unsigned>(byte_rate) : rate * a, 4); le(b, a, 2); le(b, bits, 2);
  return b;
}
// declared < 0 writes the true length; `pad` appends the RIFF pad byte for odd chunks.
Bytes chunk(const char *id, const Bytes &body, std::int64_t declared = -1, bool pad = true) {
  Bytes c = tag(id);
  le(c, declared >= 0 ? static_cast<std::uint64_t>(declared) : body.size(), 4);
  cat(c, body);
  if (pad && (body.size() & 1)) c.push_back(0);
  return c;
}
Bytes riff(const std::vector<Bytes> &chunks, std::int64_t adjust = 0, const char *form = "WAVE") {
  Bytes body = tag(form);
  for (const auto &c : chunks) cat(body, c);
  Bytes out = tag("RIFF");
  le(out, static_cast<std::uint64_t>(static_cast<std::int64_t>(body.size()) + adjust), 4);
  cat(out, body);
  return out;
}
Bytes sine(unsigned frames, unsigned ch, unsigned bits, unsigned rate, double amplitude, double hz = 440.0) {
  Bytes pcm;
  for (unsigned i = 0; i < frames; ++i) {
    const double v = amplitude * std::sin(2.0 * 3.14159265358979323846 * hz * i / rate);
    for (unsigned c = 0; c < ch; ++c) {
      if (bits == 16) le(pcm, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(v * 32767.0))), 2);
      else le(pcm, static_cast<std::uint8_t>(std::lround(128.0 + v * 127.0)), 1);
    }
  }
  return pcm;
}
Bytes wav(unsigned frames, unsigned ch = 1, unsigned bits = 16, unsigned rate = 22050, double amplitude = 0.5) {
  return riff({chunk("fmt ", fmt_body(1, ch, rate, bits)), chunk("data", sine(frames, ch, bits, rate, amplitude))});
}

fs::path root;
void put(const fs::path &p, const Bytes &b) {
  std::ofstream out(p, std::ios::binary);
  out.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
  REQUIRE(out.good());
}
std::string u8(const fs::path &p) { const auto s = p.u8string(); return std::string(s.begin(), s.end()); }
fs::path from_u8(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string write_cue(const std::string &name, const Bytes &b) { const auto p = root / from_u8(name); put(p, b); return u8(p); }
TrophySound::Settings on(const std::string &path, int master = 100, int effects = 100) { return {true, path, master, effects}; }

// ---- postmix measurement of what the mixer actually produces ----
std::atomic<int> g_peak{0}; // peak magnitude * 1e6
std::atomic<bool> g_float{false};
void postmix(void *, Uint8 *stream, int len) {
  float peak = 0;
  if (g_float.load()) {
    const auto *s = reinterpret_cast<const float *>(stream);
    for (int i = 0; i < len / 4; ++i) peak = std::max(peak, std::fabs(s[i]));
  } else {
    const auto *s = reinterpret_cast<const Sint16 *>(stream);
    for (int i = 0; i < len / 2; ++i) peak = std::max(peak, std::fabs(s[i] / 32768.0f));
  }
  const int scaled = static_cast<int>(peak * 1e6f);
  int old = g_peak.load();
  while (scaled > old && !g_peak.compare_exchange_weak(old, scaled)) {}
}
float measure(int settle_ms = 50, int window_ms = 120) {
  SDL_Delay(static_cast<Uint32>(settle_ms)); g_peak = 0; SDL_Delay(static_cast<Uint32>(window_ms));
  return static_cast<float>(g_peak.load()) / 1e6f;
}
bool wait_for(const std::function<bool()> &pred, int ms = 3000) {
  for (int i = 0; i < ms / 5; ++i) { if (pred()) return true; SDL_Delay(5); }
  return pred();
}
bool open_mixer(int rate, Uint16 format, int channels) {
  g_float = format == AUDIO_F32SYS;
  if (Mix_OpenAudio(rate, format, channels, 512) != 0) return false;
  Mix_SetPostMix(postmix, nullptr);
  return true;
}
void close_mixer() { Mix_SetPostMix(nullptr, nullptr); Mix_HaltChannel(-1); Mix_CloseAudio(); }

// A silent looping "other cue" with its own volume, standing in for unrelated effects.
struct Other {
  std::vector<Uint8> pcm = std::vector<Uint8>(4096 * 4, 0);
  Mix_Chunk *chunk = Mix_QuickLoad_RAW(pcm.data(), static_cast<Uint32>(pcm.size()));
  int channel = -1;
  Other() { Mix_VolumeChunk(chunk, 77); }
  ~Other() { if (Mix_QuerySpec(nullptr, nullptr, nullptr) > 0) Mix_HaltChannel(-1); Mix_FreeChunk(chunk); }
  int play(int on_channel = -1) { channel = Mix_PlayChannel(on_channel, chunk, -1); return channel; }
  bool playing() const { return channel >= 0 && Mix_Playing(channel) && Mix_GetChunk(channel) == chunk; }
};

// ---- source preservation ----
std::uint64_t fnv(const Bytes &b) { std::uint64_t h = 1469598103934665603ull; for (auto c : b) { h ^= c; h *= 1099511628211ull; } return h; }
Bytes slurp(const fs::path &p) { std::ifstream in(p, std::ios::binary); return Bytes(std::istreambuf_iterator<char>(in), {}); }
struct Snapshot {
  std::uint64_t hash; fs::file_time_type mtime; std::uintmax_t size; std::set<std::string> names;
  static Snapshot of(const fs::path &file) {
    Snapshot s{fnv(slurp(file)), fs::last_write_time(file), fs::file_size(file), {}};
    for (const auto &e : fs::directory_iterator(file.parent_path())) s.names.insert(u8(e.path().filename()));
    return s;
  }
  bool operator==(const Snapshot &o) const { return hash == o.hash && mtime == o.mtime && size == o.size && names == o.names; }
};

void expect(const std::string &path, Cue status, const char *label) {
  const auto r = TrophySound::inspect(path);
  if (r.status != status) {
    throw std::runtime_error(std::string("inspect ") + label + ": got '" + TrophySound::describe(r.status) + "' expected '" + TrophySound::describe(status) + "'");
  }
  REQUIRE(r.decoded == false);
}

// ============================ validation ============================
void validation() {
  const auto good = [&](const char *name, const Bytes &b, Cue s = Cue::ready) { expect(write_cue(name, b), s, name); };
  good("ok-mono16.wav", wav(2205));
  {
    const auto r = TrophySound::inspect(write_cue("ok-info.wav", wav(11025, 2, 16, 44100)));
    REQUIRE(r.ok() && r.sample_rate == 44100 && r.channels == 2 && r.bits_per_sample == 16 && r.duration_ms == 250 && r.file_bytes == 44 + 11025 * 4);
  }
  good("ok-mono8.wav", wav(800, 1, 8, 8000));
  good("ok-stereo-96k.wav", wav(9600, 2, 16, 96000));
  good("five-seconds-8k.wav", wav(40000, 1, 8, 8000));
  good("five-seconds-48k-stereo.wav", wav(240000, 2, 16, 48000));
  good("five-seconds-plus-one.wav", wav(40001, 1, 8, 8000), Cue::too_long);
  {
    const auto r = TrophySound::inspect(write_cue("too-long-info.wav", wav(48001, 1, 16, 8000)));
    REQUIRE(r.status == Cue::too_long && r.duration_ms == 6000);
  }
  // Container oddities that are still valid PCM.
  good("list-before-data.wav", riff({chunk("fmt ", fmt_body(1, 1, 22050, 16)), chunk("LIST", Bytes{1, 2, 3, 4, 5}), chunk("data", sine(100, 1, 16, 22050, 0.4))}));
  good("fmt-extra-bytes.wav", riff({chunk("fmt ", [] { auto b = fmt_body(1, 1, 22050, 16); le(b, 0, 2); return b; }()), chunk("data", sine(100, 1, 16, 22050, 0.4))}));
  {
    // Odd-length final data chunk (8-bit mono) whose pad byte is absent from the RIFF extent.
    Bytes d = sine(101, 1, 8, 8000, 0.4);
    Bytes w = riff({chunk("fmt ", fmt_body(1, 1, 8000, 8)), chunk("data", d, -1, false)});
    good("odd-data-no-pad.wav", w);
  }
  {
    Bytes w = wav(100); for (int i = 0; i < 37; ++i) w.push_back(0xAA); // trailing junk outside the RIFF extent
    good("trailing-junk.wav", w);
  }
  {
    // Exactly the 2 MiB limit is accepted, one byte more is rejected before any decode.
    const Bytes data = sine(240000, 2, 16, 96000 / 2, 0.3);
    Bytes w = riff({chunk("fmt ", fmt_body(1, 2, 48000, 16)), chunk("data", data)});
    const std::size_t room = TrophySound::max_file_bytes - w.size();
    Bytes junk(room - 8, 0);
    Bytes filled = riff({chunk("fmt ", fmt_body(1, 2, 48000, 16)), chunk("junk", junk), chunk("data", data)});
    REQUIRE(filled.size() == TrophySound::max_file_bytes);
    good("exact-2mib.wav", filled);
    filled.push_back(0);
    good("2mib-plus-one.wav", filled, Cue::too_large);
  }
  // Unsupported formats.
  const auto fmt_only = [&](unsigned f, unsigned ch, unsigned rate, unsigned bits, int align = -1) {
    return riff({chunk("fmt ", fmt_body(f, ch, rate, bits, align)), chunk("data", Bytes(400, 1))});
  };
  good("adpcm.wav", fmt_only(2, 1, 22050, 4), Cue::unsupported_format);
  good("float.wav", fmt_only(3, 1, 22050, 32), Cue::unsupported_format);
  good("extensible.wav", fmt_only(0xFFFE, 1, 22050, 16), Cue::unsupported_format);
  good("mp3-tag.wav", fmt_only(0x55, 2, 44100, 0), Cue::unsupported_format);
  good("24bit.wav", fmt_only(1, 1, 22050, 24), Cue::unsupported_format);
  good("12bit.wav", fmt_only(1, 1, 22050, 12), Cue::unsupported_format);
  good("zero-ch.wav", fmt_only(1, 0, 22050, 16, 2), Cue::unsupported_format);
  good("three-ch.wav", fmt_only(1, 3, 22050, 16), Cue::unsupported_format);
  good("rate-low.wav", fmt_only(1, 1, 7999, 16), Cue::unsupported_format);
  good("rate-high.wav", fmt_only(1, 1, 96001, 16), Cue::unsupported_format);
  good("rate-zero.wav", fmt_only(1, 1, 0, 16), Cue::unsupported_format);
  // Malformed / truncated.
  good("align-lie.wav", fmt_only(1, 1, 22050, 16, 4), Cue::malformed);
  good("align-zero.wav", fmt_only(1, 1, 22050, 16, 0), Cue::malformed);
  good("empty-data.wav", riff({chunk("fmt ", fmt_body(1, 1, 22050, 16)), chunk("data", Bytes{})}), Cue::malformed);
  good("partial-frame.wav", riff({chunk("fmt ", fmt_body(1, 1, 22050, 16)), chunk("data", Bytes(401, 1))}), Cue::malformed);
  good("no-data.wav", riff({chunk("fmt ", fmt_body(1, 1, 22050, 16))}), Cue::malformed);
  good("no-fmt.wav", riff({chunk("data", Bytes(400, 1))}), Cue::malformed);
  good("data-before-fmt.wav", riff({chunk("data", Bytes(400, 1)), chunk("fmt ", fmt_body(1, 1, 22050, 16))}), Cue::malformed);
  good("two-fmt.wav", riff({chunk("fmt ", fmt_body(1, 1, 22050, 16)), chunk("fmt ", fmt_body(1, 1, 22050, 16)), chunk("data", Bytes(400, 1))}), Cue::malformed);
  good("two-data.wav", riff({chunk("fmt ", fmt_body(1, 1, 22050, 16)), chunk("data", Bytes(400, 1)), chunk("data", Bytes(400, 1))}), Cue::malformed);
  good("fmt-short.wav", riff({chunk("fmt ", Bytes(15, 0)), chunk("data", Bytes(400, 1))}), Cue::malformed);
  good("riff-size-lie.wav", riff({chunk("fmt ", fmt_body(1, 1, 22050, 16)), chunk("data", Bytes(400, 1))}, 1000), Cue::malformed);
  good("riff-size-zero.wav", riff({chunk("fmt ", fmt_body(1, 1, 22050, 16)), chunk("data", Bytes(400, 1))}, -400), Cue::malformed);
  good("data-size-lie.wav", riff({chunk("fmt ", fmt_body(1, 1, 22050, 16)), chunk("data", Bytes(400, 1), 4000)}), Cue::malformed);
  good("data-size-max.wav", riff({chunk("fmt ", fmt_body(1, 1, 22050, 16)), chunk("data", Bytes(400, 1), 0xFFFFFFFFll)}), Cue::malformed);
  good("tail-fragment.wav", [] { auto w = wav(100); for (int i = 0; i < 3; ++i) w.push_back(1); w[4] += 3; return w; }(), Cue::malformed);
  {
    Bytes w = wav(2000);
    w.resize(w.size() / 2); // file cut in half
    good("cut-in-half.wav", w, Cue::malformed);
  }
  good("not-riff.wav", [] { auto w = wav(100); w[0] = 'X'; return w; }(), Cue::not_wav);
  good("not-wave.wav", [] { auto w = wav(100); w[8] = 'X'; return w; }(), Cue::not_wav);
  good("short-file.wav", Bytes{'R', 'I', 'F', 'F', 0, 0}, Cue::not_wav);
  good("zero-file.wav", Bytes{}, Cue::not_wav);
  good("renamed.mp3", wav(100)); // extension is never trusted either way
  good("text.wav", Bytes{'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', '!', '!', '!'}, Cue::not_wav);
  // Paths.
  expect("", Cue::path_empty, "empty path");
  expect("trophy.wav", Cue::path_invalid, "relative");
  expect("../trophy.wav", Cue::path_invalid, "relative parent");
  expect(std::string("x\0y", 3), Cue::path_invalid, "NUL");
  expect(u8(root) + "/bad\xC3\x28.wav", Cue::path_invalid, "invalid UTF-8");
  expect(u8(root) + "/bad\xED\xA0\x80.wav", Cue::path_invalid, "surrogate UTF-8");
  expect(u8(root) + "/bad\xE2\x82", Cue::path_invalid, "truncated UTF-8");
  expect(u8(root) + "/", Cue::path_invalid, "directory path without file name");
  expect(u8(root / "missing.wav"), Cue::not_found, "missing");
  fs::create_directories(root / "folder.wav");
  expect(u8(root / "folder.wav"), Cue::not_regular_file, "directory");
#ifdef _WIN32
  expect("\\\\?\\" + u8(root / "ok-mono16.wav"), Cue::path_invalid, "device namespace");
  expect("\\\\localhost\\c$\\x.wav", Cue::path_invalid, "UNC");
  expect("//server/share/x.wav", Cue::path_invalid, "UNC slashes");
  expect(u8(root / "ok-mono16.wav") + ":stream", Cue::path_invalid, "alternate stream");
  expect("C:trophy.wav", Cue::path_invalid, "drive relative");
#endif
  // Unicode directory AND file name (the root itself is Unicode).
  const auto unicode = write_cue("trophy-\xE3\x83\x88\xE3\x83\xAD\xE3\x83\x95\xE3\x82\xA3\xE3\x83\xBC-\xF0\x9F\x8F\x86-\xC3\xA9.wav", wav(441));
  expect(unicode, Cue::ready, "unicode file name");
  // Validation is pure: it needs no mixer and creates no instance state.
  REQUIRE(TrophySound::inspect(unicode).file_bytes == 44 + 882);
  // Every status has actionable text.
  for (int i = 0; i <= static_cast<int>(Cue::decode_failed); ++i) REQUIRE(std::string(TrophySound::describe(static_cast<Cue>(i))).size() > 5);
  for (int i = 0; i <= static_cast<int>(Play::play_failed); ++i) REQUIRE(std::string(TrophySound::describe(static_cast<Play>(i))).size() > 5);
  REQUIRE(TrophySound::Settings{}.enabled == false);
}

// ============================ real playback ============================
void playback() {
  REQUIRE(open_mixer(44100, AUDIO_S16SYS, 2));
  const auto cue = write_cue("tone.wav", wav(22050, 1, 16, 22050, 0.5)); // 1 s, 22.05 kHz mono: resampled by the mixer
  const auto source = Snapshot::of(root / "tone.wav");

  static int hook_sentinel = 0;
  Mix_HookMusic([](void *, Uint8 *stream, int len) { SDL_memset(stream, 0, static_cast<size_t>(len)); }, &hook_sentinel);
  const int music_volume = Mix_VolumeMusic(-1);
  const int channels = Mix_AllocateChannels(-1);
  Other other; other.play();
  const int other_channel_volume = Mix_Volume(other.channel, -1);
  REQUIRE(other.playing());

  {
    TrophySound sound;
    // Default: Off, reads nothing, plays nothing, but consumes.
    REQUIRE(sound.play_once(1) == Play::off && sound.last_serial() == 1 && !sound.active());
    auto r = sound.configure(on(cue));
    REQUIRE(r.ok() && r.decoded && r.duration_ms == 1000 && sound.cue().decoded);
    REQUIRE(sound.volume() == 128);
    REQUIRE(measure() < 0.001f);                                  // silence before play: postmix hook is live
    REQUIRE(sound.play_once(2) == Play::played);
    REQUIRE(sound.active() && sound.channel() >= 0 && sound.channel() != other.channel);
    REQUIRE(Mix_Playing(-1) == 2 && Mix_GetChunk(sound.channel()) != nullptr);
    const float full = measure(30, 150);
    REQUIRE(full > 0.35f && full < 0.6f);                         // nonzero post-mix samples from the real mixer
    REQUIRE(sound.active());                                      // 1 s cue still running: measure() consumed ~0.2 s
    // Nothing global or unrelated changed.
    REQUIRE(Mix_Volume(other.channel, -1) == other_channel_volume && Mix_VolumeChunk(other.chunk, -1) == 77);
    REQUIRE(Mix_VolumeMusic(-1) == music_volume && Mix_GetMusicHookData() == &hook_sentinel && Mix_AllocateChannels(-1) == channels);
    REQUIRE(Mix_Volume(sound.channel(), -1) == MIX_MAX_VOLUME);

    // Live gain, applied to our chunk only; master*effects.
    const int ch = sound.channel();
    sound.set_volume(50, 50);
    REQUIRE(sound.volume() == 32 && sound.active() && sound.channel() == ch);
    const float quarter = measure(30, 120);
    REQUIRE(quarter > full * 0.20f && quarter < full * 0.30f);
    REQUIRE(Mix_VolumeChunk(other.chunk, -1) == 77 && Mix_Volume(other.channel, -1) == other_channel_volume);
    sound.set_volume(100, 50);
    REQUIRE(sound.volume() == 64);
    const float half = measure(30, 100);
    REQUIRE(half > full * 0.42f && half < full * 0.58f);
    // Zero stops our own cue only, keeps the other, and never restarts.
    sound.set_volume(100, 0);
    REQUIRE(sound.volume() == 0 && !sound.active() && other.playing() && Mix_Playing(ch) == 0);
    sound.set_volume(100, 100);
    REQUIRE(!sound.active() && sound.volume() == 128 && measure(80, 100) < 0.001f);
    // Clamping.
    sound.set_volume(-20, 500); REQUIRE(sound.volume() == 0);
    sound.set_volume(500, 500); REQUIRE(sound.volume() == 128);
    sound.set_volume(10, 10); REQUIRE(sound.volume() == 1);
    sound.set_volume(1, 1); REQUIRE(sound.volume() == 0);
    sound.set_volume(100, 100);

    // A new serial restarts only our own cue (never two copies).
    REQUIRE(sound.play_once(3) == Play::played && sound.play_once(4) == Play::played);
    REQUIRE(Mix_Playing(-1) == 2 && sound.active());
    sound.stop();
    REQUIRE(!sound.active() && Mix_Playing(-1) == 1 && other.playing() && measure(80, 100) < 0.001f);
    // stop() preserves the consumed serial: no replay.
    REQUIRE(sound.play_once(4) == Play::repeated && !sound.active() && sound.last_serial() == 4);
    sound.stop(); // idempotent
    REQUIRE(source == Snapshot::of(root / "tone.wav"));
  }
  REQUIRE(other.playing() && Mix_Playing(-1) == 1); // destructor halted only its own cue
  Mix_HookMusic(nullptr, nullptr);
  close_mixer();
}

void dedup() {
  REQUIRE(open_mixer(44100, AUDIO_S16SYS, 2));
  const auto cue = write_cue("dedup.wav", wav(22050));
  TrophySound sound;
  const auto playing = [] { return Mix_Playing(-1); };

  REQUIRE(sound.play_once(0) == Play::invalid_serial && sound.last_serial() == 0);
  // Disabled: consumed; enabling during the same popup does not replay it.
  REQUIRE(sound.play_once(5) == Play::off);
  REQUIRE(sound.configure(on(cue)).ok());
  REQUIRE(sound.play_once(5) == Play::repeated && playing() == 0);
  REQUIRE(sound.play_once(4) == Play::stale && sound.last_serial() == 5 && playing() == 0);
  REQUIRE(sound.play_once(0) == Play::invalid_serial);
  REQUIRE(sound.play_once(6) == Play::played && playing() == 1);
  // Promotion passive -> interactive keeps the id: exactly once.
  REQUIRE(sound.play_once(6) == Play::repeated && playing() == 1);
  sound.stop();
  REQUIRE(sound.play_once(6) == Play::repeated && playing() == 0); // cancellation never replays
  // Muted: consumed; unmuting during the popup does not replay it.
  sound.set_volume(0, 100);
  REQUIRE(sound.play_once(7) == Play::muted && playing() == 0);
  sound.set_volume(100, 100);
  REQUIRE(sound.play_once(7) == Play::repeated && playing() == 0);
  sound.set_volume(100, 0);
  REQUIRE(sound.play_once(8) == Play::muted);
  sound.set_volume(100, 100);
  // Reconfiguring (same file or another) keeps the consumed serial.
  REQUIRE(sound.configure(on(cue)).ok() && sound.last_serial() == 8);
  REQUIRE(sound.play_once(8) == Play::repeated && sound.play_once(7) == Play::stale && playing() == 0);
  // Turning the feature off consumes, then turning it on never replays that id.
  REQUIRE(sound.configure({}).status == Cue::off && !sound.cue().ok());
  REQUIRE(sound.play_once(9) == Play::off);
  REQUIRE(sound.configure(on(cue)).ok());
  REQUIRE(sound.play_once(9) == Play::repeated && playing() == 0);
  // Unavailable cue: invalid file consumes too; fixing it never replays.
  REQUIRE(sound.configure(on(u8(root / "missing.wav"))).status == Cue::not_found);
  REQUIRE(sound.play_once(10) == Play::unavailable);
  REQUIRE(sound.configure(on("")).status == Cue::path_empty && sound.play_once(11) == Play::unavailable);
  REQUIRE(sound.configure(on(cue)).ok());
  REQUIRE(sound.play_once(11) == Play::repeated && playing() == 0);
  REQUIRE(sound.play_once(12) == Play::played && playing() == 1);

  // A bad new file must not leave the old cue sounding or loaded; inspect() alone changes nothing.
  REQUIRE(TrophySound::inspect(u8(root / "missing.wav")).status == Cue::not_found && sound.active());
  const int old_channel = sound.channel();
  REQUIRE(sound.configure(on(write_cue("broken.wav", Bytes(100, 7)))).status == Cue::not_wav);
  REQUIRE(!sound.active() && Mix_Playing(old_channel) == 0 && !sound.cue().decoded);
  REQUIRE(sound.play_once(13) == Play::unavailable);
  // Replacing a playing cue with a valid one frees the old chunk only after halting it.
  REQUIRE(sound.configure(on(cue)).ok() && sound.play_once(14) == Play::played && sound.active());
  const auto second = write_cue("dedup-2.wav", wav(11025, 2, 8, 11025, 0.6));
  REQUIRE(sound.configure(on(second)).ok() && !sound.active() && playing() == 0);
  REQUIRE(sound.play_once(15) == Play::played && wait_for([&] { return !sound.active(); }, 2500));
  REQUIRE(sound.play_once(15) == Play::repeated);
  REQUIRE(sound.last_serial() == 15);
  close_mixer();
}

void channels_and_limits() {
  REQUIRE(open_mixer(44100, AUDIO_S16SYS, 2));
  const auto cue = write_cue("short.wav", wav(2205, 1, 16, 22050, 0.5)); // 0.1 s
  Other keep;
  // SDL_mixer reuses channels: after our cue ends, another cue takes our channel. We must not halt it.
  {
    TrophySound sound;
    REQUIRE(sound.configure(on(cue)).ok() && sound.play_once(1) == Play::played);
    const int channel = sound.channel();
    REQUIRE(wait_for([&] { return !sound.active(); }, 2000));
    REQUIRE(Mix_Playing(channel) == 0 && Mix_GetChunk(channel) != nullptr); // finished channel still names our chunk
    REQUIRE(keep.play(channel) == channel && keep.playing());
    REQUIRE(!sound.active());
    sound.stop();
    REQUIRE(keep.playing());
    sound.set_volume(0, 100); sound.set_volume(100, 100);
    REQUIRE(keep.playing());
    sound.device_will_close();
    REQUIRE(keep.playing() && !sound.cue().decoded);
    REQUIRE(sound.prepare() && sound.cue().decoded && keep.playing());
    REQUIRE(sound.configure(on(cue)).ok());   // frees the old chunk
    REQUIRE(keep.playing() && Mix_GetChunk(channel) == keep.chunk);
    REQUIRE(sound.play_once(2) == Play::played);
    REQUIRE(wait_for([&] { return !sound.active(); }, 2000));
    REQUIRE(keep.play(sound.channel()) == sound.channel() && keep.playing());
  }
  REQUIRE(keep.playing()); // destructor after channel reuse
  Mix_HaltChannel(-1);

  // Reserved channels are respected, and the channel count is untouched.
  {
    TrophySound sound;
    REQUIRE(sound.configure(on(cue)).ok());
    const int count = Mix_AllocateChannels(-1);
    REQUIRE(Mix_ReserveChannels(2) == 2);
    REQUIRE(sound.play_once(1) == Play::played && sound.channel() >= 2);
    sound.stop();
    // Saturated mixer: reported, consumed, never replayed later.
    Other busy;
    for (int i = 2; i < count; ++i) REQUIRE(busy.play(i) == i);
    REQUIRE(sound.play_once(2) == Play::no_channel && !sound.active());
    Mix_HaltChannel(-1);
    REQUIRE(sound.play_once(2) == Play::repeated && !sound.active());
    REQUIRE(sound.play_once(3) == Play::played && sound.channel() >= 2);
    Mix_ReserveChannels(0);
    REQUIRE(Mix_AllocateChannels(-1) == count);
  }
  close_mixer();
}

void devices() {
  const auto cue = write_cue("device.wav", wav(22050, 1, 16, 22050, 0.5));
  // Not-yet-open mixer: validated and cached, decoded later; serial consumed while closed.
  {
    TrophySound sound;
    const auto r = sound.configure(on(cue));
    REQUIRE(r.ok() && !r.decoded && !sound.prepare());
    REQUIRE(sound.play_once(1) == Play::device_closed);
    REQUIRE(open_mixer(44100, AUDIO_S16SYS, 2));
    REQUIRE(sound.play_once(1) == Play::repeated && !sound.active());
    REQUIRE(sound.prepare() && sound.cue().decoded);
    REQUIRE(sound.play_once(2) == Play::played && measure(30, 100) > 0.35f);
    // Parent-notified device change to float/48 kHz.
    sound.device_will_close();
    REQUIRE(!sound.active() && !sound.cue().decoded && sound.last_serial() == 2);
    close_mixer();
    REQUIRE(sound.play_once(3) == Play::device_closed);
    REQUIRE(open_mixer(48000, AUDIO_F32SYS, 2));
    REQUIRE(sound.prepare() && sound.cue().decoded);
    REQUIRE(sound.play_once(3) == Play::repeated && !sound.active());
    REQUIRE(sound.play_once(4) == Play::played);
    const float peak = measure(30, 150);
    REQUIRE(peak > 0.35f && peak < 0.6f);
    sound.stop();
    close_mixer();
  }
  // Mixer reconfigured WITHOUT notification: the cue is re-decoded for the new spec, no disk read.
  {
    TrophySound sound;
    REQUIRE(open_mixer(44100, AUDIO_S16SYS, 2));
    REQUIRE(sound.configure(on(cue)).ok());
    REQUIRE(sound.play_once(1) == Play::played);
    close_mixer();
    REQUIRE(!sound.active());
    fs::remove(root / "device.wav");                       // proves memory-only
    REQUIRE(open_mixer(48000, AUDIO_F32SYS, 2));
    REQUIRE(sound.play_once(2) == Play::played && sound.active());
    REQUIRE(measure(30, 150) > 0.35f);
    close_mixer();
    REQUIRE(open_mixer(22050, AUDIO_S16SYS, 1));
    REQUIRE(sound.play_once(3) == Play::played && sound.active());
    int rate = 0, ch = 0; Uint16 fmt = 0;
    REQUIRE(Mix_QuerySpec(&rate, &fmt, &ch) > 0 && measure(30, 150) > 0.35f);
    close_mixer();
  }
  // Destroying while the mixer is closed, then reopening.
  {
    put(root / "device.wav", wav(22050));
    REQUIRE(open_mixer(44100, AUDIO_S16SYS, 2));
    auto sound = std::make_unique<TrophySound>();
    REQUIRE(sound->configure(on(u8(root / "device.wav"))).ok() && sound->play_once(1) == Play::played);
    Other other; other.play();
    close_mixer();
    sound.reset();
    REQUIRE(open_mixer(44100, AUDIO_S16SYS, 2));
    TrophySound again;
    REQUIRE(again.configure(on(u8(root / "device.wav"))).ok() && again.play_once(1) == Play::played);
    REQUIRE(measure(30, 100) > 0.35f);
    close_mixer();
  }
}

void source_preserved() {
  REQUIRE(open_mixer(44100, AUDIO_S16SYS, 2));
  const auto name = write_cue("keep-\xC3\xA9.wav", wav(4410));
  const auto path = from_u8(name);
  const auto before = Snapshot::of(path);
  const auto root_mtime = fs::last_write_time(root);
  {
    TrophySound sound;
    REQUIRE(TrophySound::inspect(name).ok() && sound.configure(on(name)).ok() && sound.play_once(1) == Play::played);
    sound.stop(); sound.device_will_close(); REQUIRE(sound.prepare());
    sound.configure({});
  }
  REQUIRE(before == Snapshot::of(path));
  REQUIRE(root_mtime == fs::last_write_time(root));
  // A file that is replaced by a bigger one after validation is not re-read (no disk access in play_once).
  TrophySound sound;
  REQUIRE(sound.configure(on(name)).ok());
  put(path, Bytes(TrophySound::max_file_bytes + 10, 1));
  REQUIRE(sound.play_once(2) == Play::played && sound.active());
  REQUIRE(TrophySound::inspect(name).status == Cue::too_large);
  close_mixer();
}
} // namespace

int main() {
  SDL_SetMainReady();
  SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
  root = fs::temp_directory_path() / from_u8(std::string("openxmb-trophy-\xE3\x83\x88\xE3\x83\xAD\xE3\x83\x95\xE3\x82\xA3\xE3\x83\xBC-\xF0\x9F\x8F\x86-") + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  int status = 0;
  try {
    REQUIRE(SDL_InitSubSystem(SDL_INIT_AUDIO) == 0);
    REQUIRE(std::string(SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "") == "dummy");
    fs::create_directories(root);
    validation(); std::cout << "validation ok" << std::endl;
    playback(); std::cout << "playback ok" << std::endl;
    dedup(); std::cout << "dedup ok" << std::endl;
    channels_and_limits(); std::cout << "channels ok" << std::endl;
    devices(); std::cout << "devices ok" << std::endl;
    source_preserved(); std::cout << "source preservation ok" << std::endl;
    std::cout << "TrophySound checks passed" << std::endl;
  } catch (const std::exception &error) {
    std::cerr << "FAILED " << error.what() << '\n';
    status = 1;
  }
  Mix_SetPostMix(nullptr, nullptr);
  Mix_CloseAudio();
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
  std::error_code ec;
  fs::remove_all(root, ec);
  return status;
}
