/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025-2026 Syndromatic Ltd. All rights reserved
 * Designed by Kavish Krishnakumar in Manchester.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "openxmb/music/player.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <xmp.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <limits>
#include <mutex>
#include <numbers>
#include <stdexcept>

namespace openxmb::music {
namespace {
constexpr std::size_t sample_count = 1024;
void checked(int result, const char *operation) {
  if (result >= 0) return;
  char message[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(result, message, sizeof(message));
  throw std::runtime_error(std::string(operation) + ": " + message);
}
std::string tag(AVDictionary *dictionary, const char *key) {
  const auto *entry = av_dict_get(dictionary, key, nullptr, 0);
  return entry && entry->value ? entry->value : "";
}
Metadata probe(const std::filesystem::path &path) {
  Metadata metadata;
  metadata.title = path.stem().string();
  AVFormatContext *raw = nullptr;
  const auto utf8 = path.u8string();
  if (avformat_open_input(&raw, reinterpret_cast<const char *>(utf8.c_str()), nullptr, nullptr) >= 0) {
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

// FFmpeg is used incrementally: at most one decoded block plus a one-second
// PCM ring is retained. No temporary files or whole-track conversion is used.
struct Player::Impl {
  struct Decoder {
    AVFormatContext *format{};
    AVCodecContext *codec{};
    AVPacket *packet{};
    AVFrame *frame{};
    SwrContext *resampler{};
    xmp_context tracker{};
    bool tracker_started{};
    double tracker_time{};
    int stream{-1}, rate{}, channels{};
    double origin{}, discard_before{}, next_pts{};
    bool flushing{}, done{};
    std::vector<float> pcm;
    std::size_t offset{};
    ~Decoder() {
      if (tracker) {
        if (tracker_started) xmp_end_player(tracker);
        xmp_release_module(tracker); xmp_free_context(tracker);
      }
      swr_free(&resampler);
      av_frame_free(&frame);
      av_packet_free(&packet);
      avcodec_free_context(&codec);
      avformat_close_input(&format);
    }
    void open(const std::filesystem::path &path, int output_rate, int output_channels) {
      rate = output_rate; channels = output_channels;
      auto extension = path.extension().string();
      std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (extension == ".mod" || extension == ".xm" || extension == ".it" || extension == ".s3m") {
        // Bundled FFmpeg has no tracker demuxer. Preserve existing module
        // formats by rendering libxmp frames into the same bounded PCM ring.
        tracker = xmp_create_context();
        if (!tracker) throw std::bad_alloc();
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot open tracker module");
        // Keep C runtime FILE handles inside their originating module. The
        // callbacks also preserve native Unicode paths on Windows.
        xmp_callbacks callbacks{};
        callbacks.read_func = [](void *output, unsigned long size, unsigned long count, void *opaque) -> unsigned long {
          if (!size || count > static_cast<unsigned long>(LONG_MAX) / size) return 0;
          auto& input = *static_cast<std::ifstream *>(opaque);
          input.read(static_cast<char *>(output), static_cast<std::streamsize>(size) * count);
          return static_cast<unsigned long>(input.gcount()) / size;
        };
        callbacks.seek_func = [](void *opaque, long offset, int whence) -> int {
          auto& input = *static_cast<std::ifstream *>(opaque);
          input.clear();
          input.seekg(offset, whence == SEEK_SET ? std::ios::beg : whence == SEEK_CUR ? std::ios::cur : std::ios::end);
          return input ? 0 : -1;
        };
        callbacks.tell_func = [](void *opaque) -> long { return static_cast<long>(static_cast<std::ifstream *>(opaque)->tellg()); };
        callbacks.close_func = [](void *) -> int { return 0; };
        const int loaded = xmp_load_module_from_callbacks(tracker, &input, callbacks);
        if (loaded < 0) throw std::runtime_error("Cannot decode tracker module (libxmp " + std::to_string(loaded) + ")");
        if (xmp_start_player(tracker, rate, 0) < 0) throw std::runtime_error("Cannot start tracker PCM renderer");
        tracker_started = true;
        return;
      }
      const auto utf8 = path.u8string();
      checked(avformat_open_input(&format, reinterpret_cast<const char *>(utf8.c_str()), nullptr, nullptr), "Open music");
      checked(avformat_find_stream_info(format, nullptr), "Read music streams");
      const AVCodec *decoder{};
      stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
      checked(stream, "Find audio stream");
      codec = avcodec_alloc_context3(decoder);
      if (!codec) throw std::bad_alloc();
      checked(avcodec_parameters_to_context(codec, format->streams[stream]->codecpar), "Read audio parameters");
      codec->pkt_timebase = format->streams[stream]->time_base;
      checked(avcodec_open2(codec, decoder, nullptr), "Open audio decoder");
      origin = format->streams[stream]->start_time == AV_NOPTS_VALUE ? 0.0 :
          format->streams[stream]->start_time * av_q2d(format->streams[stream]->time_base);
      packet = av_packet_alloc(); frame = av_frame_alloc();
      if (!packet || !frame) throw std::bad_alloc();
    }
    void seek(double seconds) {
      if (tracker) {
        xmp_end_player(tracker); tracker_started = false;
        if (xmp_start_player(tracker, rate, 0) < 0) throw std::runtime_error("Cannot restart tracker PCM renderer");
        tracker_started = true;
        tracker_time = 0; discard_before = seconds; done = false;
        pcm.clear(); offset = 0;
        return;
      }
      const auto target = static_cast<int64_t>((seconds + origin) / av_q2d(format->streams[stream]->time_base));
      checked(avformat_seek_file(format, stream, INT64_MIN, target, target, AVSEEK_FLAG_BACKWARD), "Seek music");
      avcodec_flush_buffers(codec);
      av_packet_unref(packet); av_frame_unref(frame);
      swr_free(&resampler);
      pcm.clear(); offset = 0;
      flushing = done = false;
      discard_before = next_pts = seconds;
    }
    // Called only by the transport thread. receive precedes send, so no packet
    // is discarded on EAGAIN; delayed decoder and resampler samples are drained.
    bool decode() {
      pcm.clear(); offset = 0;
      if (tracker) {
        while (!done) {
          const int result = xmp_play_frame(tracker);
          if (result == -XMP_END) { done = true; break; }
          if (result < 0) throw std::runtime_error("Tracker PCM decode failed");
          xmp_frame_info frame{};
          xmp_get_frame_info(tracker, &frame);
          if (frame.loop_count > 0) { done = true; break; }
          const auto frames = static_cast<std::size_t>(frame.buffer_size) / (2 * sizeof(std::int16_t));
          const auto *input = static_cast<const std::int16_t *>(frame.buffer);
          const double start = tracker_time;
          tracker_time += static_cast<double>(frames) / rate;
          if (tracker_time <= discard_before) continue;
          const auto skip = static_cast<std::size_t>(std::clamp(std::ceil((discard_before - start) * rate - 0.00001), 0.0, static_cast<double>(frames)));
          pcm.resize((frames - skip) * channels);
          for (std::size_t i = skip; i < frames; ++i)
            for (int channel = 0; channel < channels; ++channel)
              pcm[(i-skip)*channels+channel] = channels == 1 ? (input[i*2] / 65536.0f + input[i*2+1] / 65536.0f) :
                  channel < 2 ? input[i*2+channel] / 32768.0f : 0.0f;
          if (!pcm.empty()) return true;
        }
        return false;
      }
      while (!done) {
        int result = avcodec_receive_frame(codec, frame);
        if (result == AVERROR_EOF) {
          if (resampler) {
            pcm.resize(4096u * channels);
            uint8_t *out[] = {reinterpret_cast<uint8_t *>(pcm.data())};
            int samples = swr_convert(resampler, out, 4096, nullptr, 0);
            checked(samples, "Drain audio resampler");
            pcm.resize(static_cast<std::size_t>(samples) * channels);
            if (samples) return true;
          }
          done = true;
          return false;
        }
        if (result != AVERROR(EAGAIN)) {
          checked(result, "Decode music");
          if (!resampler) {
            AVChannelLayout target{};
            av_channel_layout_default(&target, channels);
            int configured = swr_alloc_set_opts2(&resampler, &target, AV_SAMPLE_FMT_FLT, rate,
                &frame->ch_layout, static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&target);
            checked(configured, "Create music resampler");
            checked(swr_init(resampler), "Initialize music resampler");
          }
          auto delay = swr_get_delay(resampler, frame->sample_rate);
          const int capacity = static_cast<int>(av_rescale_rnd(delay + frame->nb_samples, rate, frame->sample_rate, AV_ROUND_UP));
          pcm.resize(static_cast<std::size_t>(capacity) * channels);
          uint8_t *out[] = {reinterpret_cast<uint8_t *>(pcm.data())};
          int samples = swr_convert(resampler, out, capacity, const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
          checked(samples, "Resample music");
          const auto pts = frame->best_effort_timestamp;
          const double start = pts == AV_NOPTS_VALUE ? next_pts :
              pts * av_q2d(format->streams[stream]->time_base) - origin - static_cast<double>(delay) / frame->sample_rate;
          next_pts = start + static_cast<double>(samples) / rate;
          auto skip = static_cast<std::size_t>(std::clamp(std::ceil((discard_before - start) * rate - 0.00001), 0.0, static_cast<double>(samples)));
          pcm.resize(static_cast<std::size_t>(samples) * channels);
          offset = skip * channels;
          av_frame_unref(frame);
          if (offset < pcm.size()) return true;
          continue;
        }
        if (flushing) throw std::runtime_error("Audio decoder requested input after drain");
        do {
          av_packet_unref(packet);
          result = av_read_frame(format, packet);
          if (result == AVERROR_EOF) break;
          checked(result, "Read music packet");
        } while (packet->stream_index != stream);
        if (result == AVERROR_EOF) {
          checked(avcodec_send_packet(codec, nullptr), "Drain music decoder");
          flushing = true;
        } else {
          checked(avcodec_send_packet(codec, packet), "Send music packet");
          av_packet_unref(packet);
        }
      }
      return false;
    }
  };
  Queue queue;
  Snapshot state;
  std::unique_ptr<Decoder> decoder;
  bool postmix{}, external_duck{};
  int rate{}, channels{};
  Uint16 output_format{};
  float duck_gain{1.0f};
  float output_gain{1.0f};
  std::string device_name;
  bool device_known{};
  std::atomic<float> gain{0.75f};
  // The callback only holds this mutex while copying bounded PCM. FFmpeg,
  // filesystem operations and allocations never run while it is held.
  mutable std::mutex mutex;
  std::vector<float> ring;
  std::size_t read{}, write{}, queued{};
  std::uint64_t consumed{};
  bool active{};
  double base{};
  std::array<float, sample_count> samples{};
  std::size_t sample_cursor{};
  std::uint64_t sample_generation{}, last_samples{};
  std::chrono::steady_clock::time_point last_tick = std::chrono::steady_clock::now();

  static void SDLCALL mix(int, void *buffer, int length, void *userdata) {
    auto &self = *static_cast<Impl *>(userdata);
    std::lock_guard lock(self.mutex);
    if (!self.active || self.channels <= 0 || self.ring.empty()) return;
    const auto sample_bytes = self.output_format == AUDIO_F32SYS ? sizeof(float) : sizeof(std::int16_t);
    auto frames = std::min(static_cast<std::size_t>(length) / (sample_bytes * self.channels), self.queued / self.channels);
    const float volume = self.gain.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < frames; ++i) {
      float mono = 0;
      for (int channel = 0; channel < self.channels; ++channel) {
        float value = self.ring[self.read] * volume;
        self.read = (self.read + 1) % self.ring.size();
        auto index = i * self.channels + channel;
        if (self.output_format == AUDIO_F32SYS) {
          auto *output = static_cast<float *>(buffer);
          output[index] = std::clamp(output[index] + value, -1.0f, 1.0f);
        } else {
          auto *output = static_cast<std::int16_t *>(buffer);
          const int sum = output[index] + static_cast<int>(std::lrint(value * 32767.0f));
          output[index] = static_cast<std::int16_t>(std::clamp(sum, -32768, 32767));
        }
        mono += value;
      }
      self.samples[self.sample_cursor] = mono / self.channels;
      self.sample_cursor = (self.sample_cursor + 1) % sample_count;
    }
    self.queued -= frames * self.channels;
    self.consumed += frames;
    if (frames) ++self.sample_generation;
  }
  bool initialize() {
    if (!Mix_QuerySpec(&rate, &output_format, &channels) || rate <= 0 || channels <= 0 ||
        (output_format != AUDIO_S16SYS && output_format != AUDIO_F32SYS)) {
      fail("Music needs an open SDL mixer with signed 16-bit or float output.");
      return false;
    }
    if (!postmix) {
      ring.resize(static_cast<std::size_t>(rate) * channels);
      postmix = Mix_RegisterEffect(MIX_CHANNEL_POST, mix, nullptr, this) != 0;
      if (!postmix) { fail(std::string("Cannot attach music output: ") + Mix_GetError()); return false; }
    }
    return true;
  }
  void clear(double position) {
    std::lock_guard lock(mutex);
    active = false;
    read = write = queued = consumed = 0;
    base = position;
    samples.fill(0); sample_cursor = 0;
    ++sample_generation;
  }
  void release() { clear(0); decoder.reset(); }
  void fail(std::string error) {
    release();
    state.error = std::move(error);
    state.status = Status::error;
  }
  double position() const {
    std::lock_guard lock(mutex);
    return base + static_cast<double>(consumed) / rate;
  }
  void fill() {
    if (!decoder) return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(12);
    for (unsigned blocks = 0; blocks < 128; ++blocks) {
      std::size_t available{};
      {
        std::lock_guard lock(mutex);
        const auto target = static_cast<std::size_t>(rate * 0.4) * channels;
        if (queued >= target) break;
        available = target - queued;
      }
      if (decoder->offset == decoder->pcm.size() && !decoder->decode()) break;
      auto count = std::min(available, decoder->pcm.size() - decoder->offset);
      {
        std::lock_guard lock(mutex);
        for (std::size_t i = 0; i < count; ++i) {
          ring[write] = decoder->pcm[decoder->offset + i];
          write = (write + 1) % ring.size();
        }
        queued += count;
      }
      decoder->offset += count;
      if (std::chrono::steady_clock::now() >= deadline) break;
    }
  }
  bool load() {
    const auto *entry = queue.current();
    if (!entry) { release(); state.status = Status::stopped; return false; }
    if (!initialize()) return false;
    try {
      auto replacement = std::make_unique<Decoder>();
      replacement->open(entry->path, rate, channels);
      Metadata metadata;
      if (replacement->tracker) {
        xmp_module_info info{};
        xmp_get_module_info(replacement->tracker, &info);
        metadata.title = info.mod && info.mod->name[0] ? info.mod->name : entry->path.stem().string();
        metadata.codec = "tracker";
        metadata.sample_rate = rate; metadata.channels = 2;
        if (info.num_sequences && info.seq_data) metadata.duration = info.seq_data[0].duration / 1000.0;
      } else metadata = probe(entry->path);
      release(); decoder = std::move(replacement);
      state.metadata = std::move(metadata);
      state.error.clear(); state.position = 0.0;
      state.status = Status::stopped; state.spectrum.fill(0);
      ++state.generation;
      fill();
      return true;
    } catch (const std::exception &error) { fail(error.what()); return false; }
  }
  bool seek_to(double seconds) {
    if (!decoder || !std::isfinite(seconds)) return false;
    seconds = std::max(0.0, seconds);
    if (state.metadata.duration > 0) seconds = std::min(seconds, state.metadata.duration);
    bool playing = state.status == Status::playing;
    { std::lock_guard lock(mutex); active = false; }
    try {
      decoder->seek(seconds); clear(seconds); fill();
      state.position = seconds; state.spectrum.fill(0); state.error.clear();
      if (state.status == Status::ended) state.status = Status::paused;
      { std::lock_guard lock(mutex); active = playing; }
      return true;
    } catch (const std::exception &error) { fail(error.what()); return false; }
  }
  bool start() {
    if (!decoder && !load()) return false;
    if (state.status == Status::ended && !seek_to(0)) return false;
    try { fill(); } catch (const std::exception &error) { fail(error.what()); return false; }
    gain.store(state.muted ? 0.0f : state.volume * duck_gain * output_gain);
    { std::lock_guard lock(mutex); active = true; }
    state.status = Status::playing; state.error.clear();
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
  if (impl_->state.status == Status::playing) {
    { std::lock_guard lock(impl_->mutex); impl_->active = false; }
    impl_->state.position = impl_->position();
    impl_->state.status = Status::paused;
  }
}
void Player::stop() {
  { std::lock_guard lock(impl_->mutex); impl_->active = false; }
  const bool failed = impl_->state.status == Status::error;
  if (!failed) impl_->state.status = Status::stopped;
  if (impl_->decoder && !impl_->seek_to(0)) return;
  { std::lock_guard lock(impl_->mutex); impl_->active = false; }
  impl_->state.position = 0.0;
  impl_->state.status = Status::stopped;
  impl_->state.spectrum.fill(0.0f);
}
bool Player::seek(double seconds) { return impl_->seek_to(seconds); }

bool Player::next() { return impl_->queue.next() && impl_->load() && impl_->start(); }
bool Player::previous() {
  if (impl_->state.position > 3.0) return seek(0.0);
  return impl_->queue.previous() && impl_->load() && impl_->start();
}
void Player::tick() {
  auto &self = *impl_;
  if (!self.decoder) return;
  const auto now = std::chrono::steady_clock::now();
  const float elapsed = std::chrono::duration<float>(now - self.last_tick).count();
  self.last_tick = now;
  if (self.state.status == Status::playing) {
    try { self.fill(); } catch (const std::exception &error) { self.fail(error.what()); return; }
    self.state.position = self.position();
    bool empty{};
    { std::lock_guard lock(self.mutex); empty = self.queued == 0; }
    if (self.decoder->done && empty) {
      if (self.queue.next(true)) { if (self.load()) self.start(); }
      else { self.state.status = Status::ended; std::lock_guard lock(self.mutex); self.active = false; }
    }
  }
  // Interface/menu effects never attenuate the song. Only an explicit
  // external-media request may duck music; interface quieting belongs to UI.
  self.state.ducked = self.state.ducking && self.external_duck;
  const float target = self.state.ducked ? 0.25f : 1.0f;
  const float time_constant = target < self.duck_gain ? 0.06f : 0.30f;
  self.duck_gain += (target - self.duck_gain) * (1.0f - std::exp(-std::min(elapsed, 1.0f) / time_constant));
  self.state.effective_volume = self.state.muted ? 0.0f : self.state.volume * self.duck_gain * self.output_gain;
  self.gain.store(self.state.effective_volume, std::memory_order_relaxed);
  Mix_VolumeMusic(static_cast<int>(std::lround(self.state.effective_volume * MIX_MAX_VOLUME)));

  std::array<float, sample_count> samples{};
  bool fresh = false;
  {
    std::lock_guard lock(self.mutex);
    if (self.sample_generation != self.last_samples) {
      self.last_samples = self.sample_generation; fresh = true;
      for (std::size_t i = 0; i < sample_count; ++i) samples[i] = self.samples[(self.sample_cursor + i) % sample_count];
    }
  }
  if (fresh && self.state.status == Status::playing) {
    // Log-spaced frequencies in Hz, over contiguous output PCM at the real
    // device sample rate. Sound effects never enter the music analysis buffer.
    for (std::size_t band = 0; band < self.state.spectrum.size(); ++band) {
      const double frequency = 60.0 * std::pow(std::min(16000.0, self.rate * 0.45) / 60.0,
          static_cast<double>(band) / (self.state.spectrum.size() - 1));
      double real = 0.0, imaginary = 0.0;
      for (std::size_t i = 0; i < sample_count; ++i) {
        const double window = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / (sample_count - 1));
        const double angle = 2.0 * std::numbers::pi * frequency * i / self.rate;
        real += samples[i] * window * std::cos(angle);
        imaginary -= samples[i] * window * std::sin(angle);
      }
      const float level = static_cast<float>(std::clamp(std::hypot(real, imaginary) * 4.0 / sample_count, 0.0, 1.0));
      self.state.spectrum[band] = std::max(level, self.state.spectrum[band] * 0.85f);
    }
  } else if (self.state.status != Status::playing) {
    for (auto &level : self.state.spectrum) level *= 0.85f;
  }
}

void Player::shutdown() {
  if (!impl_) return;
  if (impl_->postmix) {
    Mix_UnregisterEffect(MIX_CHANNEL_POST, Impl::mix);
    impl_->postmix = false;
  }
  impl_->release();
  impl_->state.status = Status::stopped;
}
void Player::set_volume(float volume) {
  if (std::isfinite(volume)) impl_->state.volume = std::clamp(volume, 0.0f, 1.0f);
  tick();
}
void Player::set_output_gain(float gain) {
  if (!std::isfinite(gain)) return;
  auto &self = *impl_;
  self.output_gain = std::clamp(gain, 0.0f, 1.0f);
  self.state.effective_volume = self.state.muted ? 0.0f : self.state.volume * self.duck_gain * self.output_gain;
  self.gain.store(self.state.effective_volume, std::memory_order_relaxed);
}
bool Player::set_output_device(std::string device) {
  auto &self = *impl_;
  if (device == "System Default") device.clear();
  if (self.device_known && device == self.device_name && Mix_QuerySpec(nullptr, nullptr, nullptr)) return true;
  const auto old_device = self.device_name;
  const auto old_status = self.state.status;
  const double position = self.decoder ? self.position() : 0;
  if (self.postmix) {
    Mix_UnregisterEffect(MIX_CHANNEL_POST, Impl::mix);
    self.postmix = false;
  }
  self.clear(position);
  int frequency = 44100;
  Mix_QuerySpec(&frequency, nullptr, nullptr);
  Mix_CloseAudio();
  auto open = [&](const std::string &name) {
    return Mix_OpenAudioDevice(frequency, AUDIO_S16SYS, 2, 512,
        name.empty() ? nullptr : name.c_str(), 0) == 0;
  };
  bool changed = open(device);
  std::string failure;
  if (!changed) {
    failure = "Cannot open audio output " + (device.empty() ? std::string("System Default") : device) + ": " + Mix_GetError();
    if (!open(old_device)) { self.fail(failure + "; previous output also failed: " + Mix_GetError()); return false; }
  }
  self.device_name = changed ? std::move(device) : old_device;
  self.device_known = true;
  if (!self.initialize()) return false;
  if (self.decoder) {
    self.decoder->rate = self.rate;
    self.decoder->channels = self.channels;
    self.state.status = Status::paused;
    if (!self.seek_to(position)) return false;
    self.state.status = old_status;
    if (old_status == Status::playing && !self.start()) return false;
  }
  self.state.error = failure;
  return changed;
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
bool Player::enqueue(std::vector<QueueEntry> entries, bool play_next) {
  if (entries.empty()) { impl_->state.error = "No music tracks were found"; return false; }
  if (play_next && impl_->queue.current()) {
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) impl_->queue.insert_next(std::move(*it));
  } else for (auto& entry : entries) impl_->queue.append(std::move(entry));
  return true;
}
bool Player::save_playlist(const std::filesystem::path &path) {
  try { write_playlist(path, impl_->queue.entries()); return true; }
  catch (const std::exception &error) { impl_->state.error = error.what(); return false; }
}
Player &service() { static Player player; return player; }
} // namespace openxmb::music
