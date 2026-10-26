#include "openxmb/music/player.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace openxmb::music {
namespace {
constexpr std::size_t sample_count = 256;
std::string tag(AVDictionary *dictionary, const char *key) {
  const auto *entry = av_dict_get(dictionary, key, nullptr, 0);
  return entry && entry->value ? entry->value : "";
}
Metadata probe(const std::filesystem::path &path) {
  Metadata metadata;
  metadata.title = path.stem().string();
  AVFormatContext *raw = nullptr;
  if (avformat_open_input(&raw, path.string().c_str(), nullptr, nullptr) >= 0) {
    const auto close = [](AVFormatContext *value) { avformat_close_input(&value); };
    std::unique_ptr<AVFormatContext, decltype(close)> context(raw, close);
    if (avformat_find_stream_info(raw, nullptr) >= 0) {
      const int audio = av_find_best_stream(raw, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
      AVDictionary *stream_tags = audio >= 0 ? raw->streams[audio]->metadata : nullptr;
      auto value = [&](const char *key) {
        auto result = tag(raw->metadata, key);
        return result.empty() ? tag(stream_tags, key) : result;
      };
      if (auto title = value("title"); !title.empty()) metadata.title = std::move(title);
      metadata.artist = value("artist");
      metadata.album = value("album");
      metadata.genre = value("genre");
      metadata.date = value("date");
      metadata.track = value("track");
      if (raw->duration != AV_NOPTS_VALUE && raw->duration > 0)
        metadata.duration = static_cast<double>(raw->duration) / AV_TIME_BASE;
      metadata.bit_rate = raw->bit_rate;
      if (audio >= 0) {
        const auto *parameters = raw->streams[audio]->codecpar;
        metadata.codec = avcodec_get_name(parameters->codec_id);
        metadata.sample_rate = parameters->sample_rate;
        metadata.channels = parameters->ch_layout.nb_channels;
      }
      for (unsigned i = 0; i < raw->nb_streams; ++i) {
        const auto *stream = raw->streams[i];
        const auto &packet = stream->attached_pic;
        if (!(stream->disposition & AV_DISPOSITION_ATTACHED_PIC) || packet.size <= 0 || packet.size > 16 * 1024 * 1024) continue;
        if (stream->codecpar->codec_id != AV_CODEC_ID_MJPEG && stream->codecpar->codec_id != AV_CODEC_ID_PNG) continue;
        metadata.artwork = std::make_shared<const std::vector<std::uint8_t>>(packet.data, packet.data + packet.size);
        metadata.artwork_type = stream->codecpar->codec_id == AV_CODEC_ID_PNG ? "PNG" : "JPG";
        break;
      }
    }
  }
  if (!metadata.artwork) {
    for (const char *name : {"cover.jpg", "cover.png", "folder.jpg", "folder.png", "Cover.jpg"}) {
      const auto candidate = path.parent_path() / name;
      std::error_code error;
      if (std::filesystem::is_regular_file(candidate, error)) {
        metadata.artwork_path = candidate;
        break;
      }
    }
  }
  return metadata;
}
} // namespace

struct Player::Impl {
  Queue queue;
  Snapshot state;
  Mix_Music *music{};
  bool postmix{}, external_duck{}, s16_audio{};
  int channels{2};
  float duck_gain{1.0f};
  std::array<std::atomic<float>, sample_count> samples{};
  std::atomic<std::uint64_t> sample_generation{};
  std::uint64_t last_samples{};
  std::chrono::steady_clock::time_point last_tick = std::chrono::steady_clock::now();

  static void SDLCALL capture(int, void *buffer, int length, void *userdata) {
    auto &self = *static_cast<Impl *>(userdata);
    if (!self.s16_audio || self.channels <= 0) return;
    const auto *pcm = static_cast<const std::int16_t *>(buffer);
    const auto frames = static_cast<std::size_t>(length) / (sizeof(std::int16_t) * self.channels);
    if (!frames) return;
    for (std::size_t index = 0; index < sample_count; ++index) {
      const auto frame = index * frames / sample_count;
      float sum = 0.0f;
      for (int channel = 0; channel < self.channels; ++channel)
        sum += static_cast<float>(pcm[frame * self.channels + channel]) / 32768.0f;
      self.samples[index].store(sum / self.channels, std::memory_order_relaxed);
    }
    self.sample_generation.fetch_add(1, std::memory_order_release);
  }
  bool initialize() {
    int frequency{};
    Uint16 format{};
    if (!Mix_QuerySpec(&frequency, &format, &channels)) {
      fail("Audio output is unavailable. Check the selected audio device.");
      return false;
    }
    s16_audio = format == AUDIO_S16SYS;
    if (!postmix) postmix = Mix_RegisterEffect(MIX_CHANNEL_POST, capture, nullptr, this) != 0;
    return true;
  }
  void fail(std::string error) {
    release();
    state.error = std::move(error);
    state.status = Status::error;
  }
  void release() {
    if (music) {
      Mix_HaltMusic();
      Mix_FreeMusic(music);
      music = nullptr;
    }
  }
  bool load() {
    const auto *entry = queue.current();
    if (!entry) { release(); state.status = Status::stopped; return false; }
    if (!initialize()) return false;
    auto *replacement = Mix_LoadMUS(entry->path.string().c_str());
    if (!replacement) { fail("Cannot open " + entry->path.filename().string() + ": " + Mix_GetError()); return false; }
    // Validate the replacement before releasing a currently playing track.
    release();
    music = replacement;
    state.metadata = probe(entry->path);
    const double duration = Mix_MusicDuration(music);
    if (duration > 0.0) state.metadata.duration = duration;
    state.error.clear();
    state.position = 0.0;
    state.status = Status::stopped;
    ++state.generation;
    return true;
  }
  bool start() {
    if (!music && !load()) return false;
    const float gain = state.muted ? 0.0f : state.volume * duck_gain;
    Mix_VolumeMusic(static_cast<int>(std::lround(gain * MIX_MAX_VOLUME)));
    if (state.status == Status::paused) Mix_ResumeMusic();
    else if (Mix_PlayMusic(music, 0) != 0) { fail(Mix_GetError()); return false; }
    state.status = Status::playing;
    state.error.clear();
    return true;
  }
};

Player::Player() : impl_(std::make_unique<Impl>()) {}
Player::~Player() { shutdown(); }
const Snapshot &Player::snapshot() const noexcept { return impl_->state; }
const Queue &Player::queue() const noexcept { return impl_->queue; }
bool Player::set_queue(std::vector<QueueEntry> entries, std::size_t selected) {
  if (entries.empty() || selected >= entries.size()) { impl_->fail("Music queue is empty or selection is invalid"); return false; }
  impl_->queue.assign(std::move(entries), selected);
  return impl_->load() && impl_->start();
}
bool Player::open(const std::filesystem::path &path) {
  try {
    if (supported_playlist(path)) return set_queue(read_playlist(path));
    if (!supported_audio(path)) { impl_->fail("Unsupported music format"); return false; }
    std::vector<QueueEntry> entries;
    std::error_code error;
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    for (std::filesystem::directory_iterator it(parent, error), end; !error && it != end; it.increment(error)) {
      if (entries.size() >= 10000) break;
      if (it->is_regular_file(error) && supported_audio(it->path()))
        entries.push_back({it->path(), it->path().stem().string()});
    }
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.path.filename() < b.path.filename(); });
    const auto normalized = std::filesystem::absolute(path).lexically_normal();
    auto found = std::find_if(entries.begin(), entries.end(), [&](const auto &entry) { return std::filesystem::absolute(entry.path).lexically_normal() == normalized; });
    if (found == entries.end()) { entries.insert(entries.begin(), {path, path.stem().string()}); found = entries.begin(); }
    const auto selected = static_cast<std::size_t>(found - entries.begin());
    return set_queue(std::move(entries), selected);
  } catch (const std::exception &error) { impl_->fail(error.what()); return false; }
}
bool Player::select(std::size_t index) {
  return impl_->queue.select(index) && impl_->load() && impl_->start();
}
bool Player::play() { return impl_->start(); }
void Player::pause() {
  if (impl_->state.status == Status::playing) { Mix_PauseMusic(); impl_->state.status = Status::paused; }
}
void Player::stop() {
  Mix_HaltMusic();
  impl_->state.position = 0.0;
  impl_->state.status = Status::stopped;
  impl_->state.spectrum.fill(0.0f);
}
bool Player::seek(double seconds) {
  if (!impl_->music || !std::isfinite(seconds)) return false;
  const double duration = impl_->state.metadata.duration;
  seconds = std::max(0.0, seconds);
  if (duration > 0.0) seconds = std::min(seconds, std::max(0.0, duration - 0.01));
  const bool stopped = impl_->state.status == Status::stopped || impl_->state.status == Status::ended;
  if (stopped && !impl_->start()) return false;
  if (Mix_SetMusicPosition(seconds) != 0) {
    impl_->state.error = "Seeking is unavailable for this track: " + std::string(Mix_GetError());
    if (stopped) stop();
    return false;
  }
  const double actual = Mix_GetMusicPosition(impl_->music);
  impl_->state.position = actual >= 0.0 ? actual : seconds;
  impl_->state.error.clear();
  return true;
}
bool Player::next() { return impl_->queue.next() && impl_->load() && impl_->start(); }
bool Player::previous() {
  if (impl_->state.position > 3.0) return seek(0.0);
  return impl_->queue.previous() && impl_->load() && impl_->start();
}
void Player::tick() {
  auto &self = *impl_;
  if (!self.music) return;
  const auto now = std::chrono::steady_clock::now();
  const float elapsed = std::chrono::duration<float>(now - self.last_tick).count();
  self.last_tick = now;
  if (self.state.status == Status::playing) {
    if (!Mix_PlayingMusic()) {
      if (self.queue.next(true)) {
        if (self.load()) self.start();
      } else {
        self.state.status = Status::ended;
        self.state.position = self.state.metadata.duration;
      }
    } else {
      const double position = Mix_GetMusicPosition(self.music);
      if (position >= 0.0) self.state.position = position;
    }
  }
  self.state.ducked = self.state.ducking && (self.external_duck || Mix_Playing(-1) > 0);
  const float target = self.state.ducked ? 0.25f : 1.0f;
  const float time_constant = target < self.duck_gain ? 0.06f : 0.30f;
  self.duck_gain += (target - self.duck_gain) * (1.0f - std::exp(-std::min(elapsed, 1.0f) / time_constant));
  self.state.effective_volume = self.state.muted ? 0.0f : self.state.volume * self.duck_gain;
  Mix_VolumeMusic(static_cast<int>(std::lround(self.state.effective_volume * MIX_MAX_VOLUME)));

  const auto generation = self.sample_generation.load(std::memory_order_acquire);
  if (generation != self.last_samples && self.state.status == Status::playing) {
    self.last_samples = generation;
    std::array<float, sample_count> samples;
    for (std::size_t i = 0; i < sample_count; ++i)
      samples[i] = self.samples[i].load(std::memory_order_relaxed);
    for (std::size_t band = 0; band < self.state.spectrum.size(); ++band) {
      const double frequency = 1.0 + band * band * 0.16;
      double real = 0.0, imaginary = 0.0;
      for (std::size_t i = 0; i < sample_count; ++i) {
        const double window = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / (sample_count - 1));
        const double angle = 2.0 * std::numbers::pi * frequency * i / sample_count;
        real += samples[i] * window * std::cos(angle);
        imaginary -= samples[i] * window * std::sin(angle);
      }
      const float level = static_cast<float>(std::clamp(std::sqrt(real * real + imaginary * imaginary) / 16.0, 0.0, 1.0));
      self.state.spectrum[band] = std::max(level, self.state.spectrum[band] * 0.85f);
    }
  } else if (self.state.status != Status::playing) {
    for (auto &level : self.state.spectrum) level *= 0.85f;
  }
}
void Player::shutdown() {
  if (!impl_) return;
  if (impl_->postmix) {
    Mix_UnregisterEffect(MIX_CHANNEL_POST, Impl::capture);
    impl_->postmix = false;
  }
  impl_->release();
  impl_->state.status = Status::stopped;
}
void Player::set_volume(float volume) {
  if (std::isfinite(volume)) impl_->state.volume = std::clamp(volume, 0.0f, 1.0f);
  tick();
}
void Player::set_muted(bool enabled) { impl_->state.muted = enabled; tick(); }
void Player::set_background_enabled(bool enabled) { impl_->state.background = enabled; }
void Player::set_ducking_enabled(bool enabled) { impl_->state.ducking = enabled; }
void Player::set_external_duck(bool active) { impl_->external_duck = active; }
void Player::set_visualizer(Visualizer visualizer) { impl_->state.visualizer = visualizer; }
void Player::set_shuffle(bool enabled) { impl_->queue.set_shuffle(enabled); }
void Player::set_repeat(Repeat repeat) { impl_->queue.set_repeat(repeat); }
bool Player::remove(std::size_t index) {
  const bool current = impl_->queue.current_index() == index;
  if (!impl_->queue.erase(index)) return false;
  if (current) { if (impl_->queue.current()) return impl_->load() && impl_->start(); stop(); impl_->release(); }
  return true;
}
bool Player::move(std::size_t from, std::size_t to) { return impl_->queue.move(from, to); }
bool Player::append(const std::filesystem::path &path) {
  try {
    if (supported_playlist(path)) {
      for (auto &entry : read_playlist(path)) impl_->queue.append(std::move(entry));
    } else if (supported_audio(path)) impl_->queue.append({path, path.stem().string()});
    else return false;
    return true;
  } catch (const std::exception &error) { impl_->state.error = error.what(); return false; }
}
bool Player::save_playlist(const std::filesystem::path &path) {
  try { write_playlist(path, impl_->queue.entries()); return true; }
  catch (const std::exception &error) { impl_->state.error = error.what(); return false; }
}
Player &service() { static Player player; return player; }
} // namespace openxmb::music
