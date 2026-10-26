#include "openxmb/desktop/preview_audio.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace openxmb::desktop {
struct PreviewAudio::Impl {
  struct Request {
    std::filesystem::path path;
    int frequency{}, channels{};
    bool floating{};
    std::uint64_t generation{};
  };
  struct Decoded {
    std::vector<std::uint8_t> pcm;
    std::string error;
    std::uint64_t generation{};
  };
  std::mutex mutex;
  std::condition_variable wake;
  std::optional<Request> request;
  std::optional<Decoded> decoded;
  std::atomic<std::uint64_t> generation{};
  std::jthread thread;
  std::vector<std::uint8_t> playing_pcm;
  Mix_Chunk *chunk{};
  int channel{-1};
  int previous_volume{MIX_MAX_VOLUME};
  float volume{0.5f};
  bool loading{};
  std::string error;

  Impl() : thread([this](std::stop_token stop) { run(stop); }) {}
  ~Impl() {
    generation.fetch_add(1);
    thread.request_stop(); wake.notify_all(); thread.join();
    halt();
  }
  void halt() {
    if (channel >= 0) { Mix_HaltChannel(channel); Mix_Volume(channel, previous_volume); channel = -1; }
    if (chunk) { Mix_FreeChunk(chunk); chunk = nullptr; }
    playing_pcm.clear();
  }
  struct Cancellation {
    Impl *self;
    std::uint64_t generation;
    std::stop_token stop;
  };
  static int cancelled(void *opaque) {
    auto &request = *static_cast<Cancellation *>(opaque);
    return request.stop.stop_requested() || request.self->generation.load() != request.generation;
  }
  std::vector<std::uint8_t> decode(const Request &request, std::stop_token stop) {
    Cancellation cancellation{this, request.generation, stop};
    AVFormatContext *raw = avformat_alloc_context();
    if (!raw) throw std::runtime_error("Cannot allocate preview decoder");
    raw->interrupt_callback = {cancelled, &cancellation};
    const auto opened = avformat_open_input(&raw, request.path.string().c_str(), nullptr, nullptr);
    if (opened < 0) { if (raw) avformat_close_input(&raw); throw std::runtime_error("Cannot open preview music: " + request.path.filename().string()); }
    const auto close_format = [](AVFormatContext *value) { avformat_close_input(&value); };
    std::unique_ptr<AVFormatContext, decltype(close_format)> format(raw, close_format);
    if (avformat_find_stream_info(raw, nullptr) < 0) throw std::runtime_error("Cannot read preview music stream");
    const AVCodec *decoder{};
    const int stream = av_find_best_stream(raw, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (stream < 0 || !decoder) throw std::runtime_error("Preview music has no supported audio stream");
    const auto free_codec = [](AVCodecContext *value) { avcodec_free_context(&value); };
    std::unique_ptr<AVCodecContext, decltype(free_codec)> codec(avcodec_alloc_context3(decoder), free_codec);
    if (!codec || avcodec_parameters_to_context(codec.get(), raw->streams[stream]->codecpar) < 0 || avcodec_open2(codec.get(), decoder, nullptr) < 0)
      throw std::runtime_error("Cannot initialize preview music decoder");
    if (codec->sample_rate <= 0 || codec->ch_layout.nb_channels <= 0) throw std::runtime_error("Preview music has invalid sample format");
    AVChannelLayout layout;
    av_channel_layout_default(&layout, request.channels);
    SwrContext *resampler{};
    const int configured = swr_alloc_set_opts2(&resampler, &layout, request.floating ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_S16, request.frequency,
        &codec->ch_layout, codec->sample_fmt, codec->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&layout);
    const auto free_resampler = [](SwrContext *value) { swr_free(&value); };
    std::unique_ptr<SwrContext, decltype(free_resampler)> conversion(resampler, free_resampler);
    if (configured < 0 || !resampler || swr_init(resampler) < 0) throw std::runtime_error("Cannot convert preview music for audio output");
    const auto free_packet = [](AVPacket *value) { av_packet_free(&value); };
    const auto free_frame = [](AVFrame *value) { av_frame_free(&value); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    std::unique_ptr<AVFrame, decltype(free_frame)> frame(av_frame_alloc(), free_frame);
    if (!packet || !frame) throw std::runtime_error("Cannot allocate preview music frames");
    const std::size_t frame_bytes = request.channels * (request.floating ? sizeof(float) : sizeof(std::int16_t));
    const std::size_t maximum = std::min<std::size_t>(32 * 1024 * 1024, 32ULL * request.frequency * frame_bytes);
    std::vector<std::uint8_t> result;
    auto receive = [&] {
      while (!cancelled(&cancellation) && result.size() < maximum) {
        const int received = avcodec_receive_frame(codec.get(), frame.get());
        if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return;
        if (received < 0) throw std::runtime_error("Preview music decoding failed");
        const auto capacity = av_rescale_rnd(swr_get_delay(resampler, codec->sample_rate) + frame->nb_samples,
          request.frequency, codec->sample_rate, AV_ROUND_UP);
        if (capacity <= 0 || capacity > 262144) throw std::runtime_error("Preview music frame exceeds decoding limits");
        std::vector<std::uint8_t> converted(static_cast<std::size_t>(capacity) * frame_bytes);
        auto *output = converted.data();
        const auto count = swr_convert(resampler, &output, static_cast<int>(capacity), const_cast<const std::uint8_t **>(frame->extended_data), frame->nb_samples);
        av_frame_unref(frame.get());
        if (count < 0) throw std::runtime_error("Preview music sample conversion failed");
        const auto bytes = std::min(maximum - result.size(), static_cast<std::size_t>(count) * frame_bytes);
        result.insert(result.end(), converted.begin(), converted.begin() + static_cast<std::ptrdiff_t>(bytes));
      }
    };
    while (!cancelled(&cancellation) && result.size() < maximum) {
      const int read = av_read_frame(raw, packet.get());
      if (read < 0) { avcodec_send_packet(codec.get(), nullptr); receive(); break; }
      if (packet->stream_index == stream) {
        const auto sent = avcodec_send_packet(codec.get(), packet.get());
        if (sent < 0 && sent != AVERROR(EAGAIN)) throw std::runtime_error("Cannot submit preview music packet");
        receive();
      }
      av_packet_unref(packet.get());
    }
    if (cancelled(&cancellation)) return {};
    if (result.empty()) throw std::runtime_error("Preview music contains no decoded audio");
    return result;
  }
  void run(std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::optional<Request> work;
      {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return stop.stop_requested() || request.has_value(); });
        if (stop.stop_requested()) break;
        work = std::move(request); request.reset();
      }
      Decoded value; value.generation = work->generation;
      try { value.pcm = decode(*work, stop); }
      catch (const std::exception &exception) { value.error = exception.what(); }
      std::lock_guard lock(mutex);
      if (generation.load() == value.generation) decoded = std::move(value);
    }
  }
};

PreviewAudio::PreviewAudio() : impl_(std::make_unique<Impl>()) {}
PreviewAudio::~PreviewAudio() = default;
void PreviewAudio::set_path(const std::filesystem::path &path, float volume) {
  stop();
  if (path.empty()) return;
  int frequency{}, channels{}; Uint16 format{};
  if (!Mix_QuerySpec(&frequency, &format, &channels) || (format != AUDIO_S16SYS && format != AUDIO_F32SYS)) {
    impl_->error = "Preview audio output is unavailable"; return;
  }
  impl_->volume = std::clamp(volume, 0.0f, 1.0f);
  impl_->loading = true;
  std::lock_guard lock(impl_->mutex);
  impl_->request = Impl::Request{path, frequency, channels, format == AUDIO_F32SYS, impl_->generation.load()};
  impl_->wake.notify_one();
}
void PreviewAudio::tick() {
  std::optional<Impl::Decoded> result;
  { std::lock_guard lock(impl_->mutex); result = std::move(impl_->decoded); impl_->decoded.reset(); }
  if (!result || result->generation != impl_->generation.load()) return;
  impl_->loading = false;
  impl_->error = std::move(result->error);
  if (result->pcm.empty()) return;
  impl_->halt();
  impl_->playing_pcm = std::move(result->pcm);
  impl_->chunk = Mix_QuickLoad_RAW(impl_->playing_pcm.data(), static_cast<Uint32>(impl_->playing_pcm.size()));
  if (!impl_->chunk) { impl_->error = Mix_GetError(); return; }
  impl_->channel = Mix_PlayChannel(-1, impl_->chunk, -1);
  if (impl_->channel < 0) { impl_->error = Mix_GetError(); impl_->halt(); return; }
  impl_->previous_volume = Mix_Volume(impl_->channel, -1);
  Mix_Volume(impl_->channel, static_cast<int>(impl_->volume * MIX_MAX_VOLUME));
}
void PreviewAudio::stop() {
  impl_->generation.fetch_add(1);
  { std::lock_guard lock(impl_->mutex); impl_->request.reset(); impl_->decoded.reset(); }
  impl_->halt(); impl_->loading = false; impl_->error.clear();
}
bool PreviewAudio::active() const noexcept { return impl_->channel >= 0 && Mix_Playing(impl_->channel); }
bool PreviewAudio::loading() const noexcept { return impl_->loading; }
std::size_t PreviewAudio::decoded_bytes() const noexcept { return impl_->playing_pcm.size(); }
std::string PreviewAudio::error() const { return impl_->error; }
} // namespace openxmb::desktop
