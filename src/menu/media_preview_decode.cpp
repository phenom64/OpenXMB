#include "media_preview_decode.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>

namespace openxmb::menu_preview {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::int64_t byte_limit = 32LL * 1024 * 1024;
constexpr std::int64_t pixel_limit = 32LL * 1024 * 1024;
constexpr int packet_limit = 512;

// Native filesystem paths avoid Windows narrow-codepage conversion, and custom
// IO prevents demuxers from opening URLs. The budget counts reads across seeks.
struct Input {
    std::ifstream file;
    Clock::time_point deadline;
    std::int64_t& remaining;
    std::int64_t length = 0;
    AVIOContext* io = nullptr;
    AVFormatContext* format = nullptr;

    Input(const std::filesystem::path& path, Clock::time_point until, std::int64_t& bytes_left)
        : deadline(until), remaining(bytes_left) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error) return;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size == 0 || size > static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max())) return;
        length = static_cast<std::int64_t>(size);
        file.open(path, std::ios::binary);
        if (!file) return;
        auto* buffer = static_cast<unsigned char*>(av_malloc(32768));
        if (!buffer) return;
        io = avio_alloc_context(buffer, 32768, 0, this, read, nullptr, seek);
        if (!io) { av_free(buffer); return; }
        format = avformat_alloc_context();
        if (!format) return;
        format->pb = io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        format->interrupt_callback = {interrupt, this};
        format->io_open = deny_io;
        format->probesize = 1024 * 1024;
        format->max_analyze_duration = AV_TIME_BASE / 2;
        format->max_probe_packets = 128;
        format->max_streams = 32;
        AVDictionary* options = nullptr;
        // In particular, do not let playlist/container demuxers resolve external
        // files, network URLs, or nested protocols through FFmpeg's default IO.
        av_dict_set(&options, "protocol_whitelist", "", 0);
        av_dict_set(&options, "format_whitelist",
                    "mp3,mov,flac,ogg,wav,matroska,webm,avi,mpeg,mpegts,asf,aac,"
                    "image2,image2pipe,png_pipe,jpeg_pipe,bmp_pipe,webp_pipe,gif,tiff_pipe", 0);
        int status = avformat_open_input(&format, nullptr, nullptr, &options);
        av_dict_free(&options);
        if (status < 0) return;
        // Stream probing may itself open a decoder; apply the same pixel/thread
        // limits there as on the decoder used to produce the returned bitmap.
        std::array<AVDictionary*, 32> probe_options{};
        const auto streams = format->nb_streams;
        for (unsigned i = 0; i < streams; ++i) {
            av_dict_set(&probe_options[i], "max_pixels", "33554432", 0);
            av_dict_set(&probe_options[i], "threads", "1", 0);
        }
        const int info = avformat_find_stream_info(format, probe_options.data());
        for (auto*& entry : probe_options) av_dict_free(&entry);
        if (info < 0) {
            avformat_close_input(&format);
        }
    }
    ~Input() {
        if (format) avformat_close_input(&format);
        if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    }
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    static int deny_io(AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**) {
        return AVERROR(EPERM);
    }
    static int interrupt(void* opaque) {
        return Clock::now() >= static_cast<Input*>(opaque)->deadline;
    }
    static int read(void* opaque, std::uint8_t* buffer, int count) {
        auto& self = *static_cast<Input*>(opaque);
        if (interrupt(opaque)) return AVERROR_EXIT;
        if (self.remaining <= 0) return AVERROR(EIO);
        count = static_cast<int>(std::min<std::int64_t>(count, self.remaining));
        self.file.read(reinterpret_cast<char*>(buffer), count);
        const auto got = self.file.gcount();
        self.remaining -= got;
        return got > 0 ? static_cast<int>(got) : AVERROR_EOF;
    }
    static std::int64_t seek(void* opaque, std::int64_t offset, int whence) {
        auto& self = *static_cast<Input*>(opaque);
        if (interrupt(opaque)) return AVERROR_EXIT;
        if (whence & AVSEEK_SIZE) return self.length;
        whence &= ~AVSEEK_FORCE;
        self.file.clear();
        std::int64_t base = 0;
        if (whence == SEEK_CUR) base = static_cast<std::int64_t>(self.file.tellg());
        else if (whence == SEEK_END) base = self.length;
        else if (whence != SEEK_SET) return AVERROR(EINVAL);
        if (base < 0 || offset < -base || offset > self.length - base) return AVERROR(EINVAL);
        self.file.seekg(base + offset, std::ios::beg);
        return self.file ? base + offset : AVERROR(EIO);
    }
};
struct CodecFree { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct FrameFree { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PacketFree { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct ScaleFree { void operator()(SwsContext* p) const { sws_freeContext(p); } };
using Codec = std::unique_ptr<AVCodecContext, CodecFree>;
using Frame = std::unique_ptr<AVFrame, FrameFree>;
using Packet = std::unique_ptr<AVPacket, PacketFree>;

photos::Bitmap rgba(const AVFrame& frame, AVRational stream_sar, int edge) {
    if (frame.width <= 0 || frame.height <= 0 ||
        static_cast<std::int64_t>(frame.width) * frame.height > pixel_limit || frame.format < 0) return {};
    const auto sar = frame.sample_aspect_ratio.num > 0 && frame.sample_aspect_ratio.den > 0
        ? frame.sample_aspect_ratio : stream_sar;
    double width = frame.width;
    if (sar.num > 0 && sar.den > 0) width *= av_q2d(sar);
    if (!std::isfinite(width) || width <= 0) return {};
    const double factor = std::min(1.0, edge / std::max(width, static_cast<double>(frame.height)));
    int w = std::clamp(static_cast<int>(std::lround(std::min<double>(edge, width * factor))), 1, edge);
    int h = std::clamp(static_cast<int>(std::lround(frame.height * factor)), 1, edge);
    photos::Bitmap result{w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w)*h*4)};
    std::unique_ptr<SwsContext, ScaleFree> scaler(sws_getContext(frame.width, frame.height,
        static_cast<AVPixelFormat>(frame.format), w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!scaler) return {};
    std::uint8_t* data[] = {result.rgba.data(), nullptr, nullptr, nullptr};
    int strides[] = {w*4, 0, 0, 0};
    if (sws_scale(scaler.get(), frame.data, frame.linesize, 0, frame.height, data, strides) != h) return {};
    return result;
}

photos::Bitmap stream_image(Input& input, AVStream& stream, int edge, bool seek_frame) {
    if (Clock::now() >= input.deadline) return {};
    const auto* decoder = avcodec_find_decoder(stream.codecpar->codec_id);
    if (!decoder) return {};
    Codec codec(avcodec_alloc_context3(decoder));
    Frame frame(av_frame_alloc());
    Packet packet(av_packet_alloc());
    if (!codec || !frame || !packet || avcodec_parameters_to_context(codec.get(), stream.codecpar) < 0) return {};
    codec->thread_count = 1;
    codec->max_pixels = pixel_limit;
    if (avcodec_open2(codec.get(), decoder, nullptr) < 0) return {};
    if (stream.disposition & AV_DISPOSITION_ATTACHED_PIC) {
        if (stream.attached_pic.size <= 0 || stream.attached_pic.size > byte_limit ||
            avcodec_send_packet(codec.get(), &stream.attached_pic) < 0) return {};
        int status = avcodec_receive_frame(codec.get(), frame.get());
        if (status == AVERROR(EAGAIN)) {
            avcodec_send_packet(codec.get(), nullptr);
            status = avcodec_receive_frame(codec.get(), frame.get());
        }
        return status >= 0 ? rgba(*frame, stream.sample_aspect_ratio, edge) : photos::Bitmap{};
    }
    std::int64_t target = AV_NOPTS_VALUE;
    const auto duration = stream.duration > 0 ? stream.duration * av_q2d(stream.time_base)
        : input.format->duration > 0 ? input.format->duration / static_cast<double>(AV_TIME_BASE) : 0;
    if (seek_frame && duration > 1.5 && stream.time_base.num > 0 && stream.time_base.den > 0) {
        auto start = stream.start_time == AV_NOPTS_VALUE ? 0 : stream.start_time;
        auto requested = start + av_rescale_q(1, AVRational{1, 1}, stream.time_base);
        if (av_seek_frame(input.format, stream.index, requested, AVSEEK_FLAG_BACKWARD) >= 0) {
            target = requested;
            avcodec_flush_buffers(codec.get());
        }
    }
    auto receive = [&]() -> photos::Bitmap {
        for (int n = 0; n < 16 && Clock::now() < input.deadline; ++n) {
            if (avcodec_receive_frame(codec.get(), frame.get()) < 0) break;
            const auto timestamp = frame->best_effort_timestamp;
            if (target == AV_NOPTS_VALUE || timestamp == AV_NOPTS_VALUE || timestamp >= target)
                return rgba(*frame, stream.sample_aspect_ratio, edge);
            av_frame_unref(frame.get());
        }
        return {};
    };
    for (int n = 0; n < packet_limit && Clock::now() < input.deadline; ++n) {
        const auto status = av_read_frame(input.format, packet.get());
        if (status < 0) {
            avcodec_send_packet(codec.get(), nullptr);
            return receive();
        }
        if (packet->size > byte_limit) return {};
        if (packet->stream_index == stream.index) {
            const auto sent = avcodec_send_packet(codec.get(), packet.get());
            av_packet_unref(packet.get());
            if (sent < 0) return {};
            auto result = receive();
            if (!result.rgba.empty()) return result;
        } else av_packet_unref(packet.get());
    }
    return {};
}

photos::Bitmap media_image(const std::filesystem::path& path, Kind kind, int edge, Clock::time_point deadline, std::int64_t& bytes_left) {
    Input input(path, deadline, bytes_left);
    if (!input.format) return {};
    // Actual motion/still video wins over cover art for video files.
    if (kind == Kind::video) {
        for (unsigned i = 0; i < input.format->nb_streams; ++i) {
            auto& stream = *input.format->streams[i];
            if (stream.codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(stream.disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                auto result = stream_image(input, stream, edge, true);
                if (!result.rgba.empty()) return result;
                break;
            }
        }
    }
    for (unsigned i = 0; i < input.format->nb_streams && Clock::now() < deadline; ++i) {
        auto& stream = *input.format->streams[i];
        if (stream.codecpar->codec_type == AVMEDIA_TYPE_VIDEO && (stream.disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            auto result = stream_image(input, stream, edge, false);
            if (!result.rgba.empty()) return result;
        }
    }
    return {};
}
} // namespace

photos::Bitmap decode(const std::filesystem::path& path, Kind kind, int maximum_edge) {
    const int edge = std::clamp(maximum_edge, 1, 256);
    if (kind == Kind::photo) return photos::load_bitmap(path, edge);
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    std::int64_t bytes_left = byte_limit;
    auto result = media_image(path, kind, edge, deadline, bytes_left);
    if (!result.rgba.empty() || kind != Kind::music) return result;
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error) return {};
    constexpr std::array<const char*, 8> names = {
        "cover.jpg", "cover.png", "folder.jpg", "folder.png", "Cover.jpg", "Cover.png", "Folder.jpg", "Folder.png"};
    for (const auto* name : names) {
        if (Clock::now() >= deadline) break;
        auto art = path.parent_path() / name;
        auto bytes = std::filesystem::file_size(art, error);
        if (error || bytes > byte_limit) continue;
        result = media_image(art, Kind::video, edge, deadline, bytes_left);
        if (!result.rgba.empty()) return result;
    }
    return {};
}
} // namespace openxmb::menu_preview


