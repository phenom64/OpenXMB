#include <openxmb/video.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace openxmb::video {
namespace {
using clock_type = std::chrono::steady_clock;

std::string av_error(int code) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    return message;
}

void require(int code, const char* operation) {
    if(code < 0) throw std::runtime_error(std::string(operation) + ": " + av_error(code));
}

std::string track_label(AVStream* stream, const char* fallback) {
    std::string label = std::string(fallback) + " " + std::to_string(stream->index + 1);
    if(auto* language = av_dict_get(stream->metadata, "language", nullptr, 0)) label += " (" + std::string(language->value) + ")";
    if(auto* title = av_dict_get(stream->metadata, "title", nullptr, 0)) label += " — " + std::string(title->value);
    return label;
}

double parse_subrip_time(std::string value) {
    std::replace(value.begin(), value.end(), ',', '.');
    int hours = 0, minutes = 0;
    double seconds = 0;
    char colon = 0;
    std::istringstream input(value);
    if(!(input >> hours >> colon) || colon != ':' || !(input >> minutes >> colon) || colon != ':' || !(input >> seconds)) return -1;
    if(hours < 0 || minutes < 0 || minutes > 59 || seconds < 0 || seconds >= 60) return -1;
    return hours * 3600.0 + minutes * 60.0 + seconds;
}

class audio_sink {
public:
    SDL_AudioDeviceID device = 0;
    SDL_AudioSpec spec{};
    std::atomic<float> gain{1.0f};
    std::atomic<std::size_t> consumed_frames{0};

    ~audio_sink() {
        if(shared_mixer) Mix_UnregisterEffect(MIX_CHANNEL_POST, mix_callback);
        if(device) SDL_CloseAudioDevice(device);
        if(initialized) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    bool valid() const { return device != 0 || shared_mixer; }

    bool open() {
        int frequency = 0, channels = 0;
        Uint16 format = 0;
        // Reuse the shell's mixer when present. This also works with backends
        // which permit only one audio device, and preserves background ducking.
        if(Mix_QuerySpec(&frequency, &format, &channels) &&
           (format == AUDIO_S16SYS || format == AUDIO_F32SYS) && channels > 0) {
            spec.freq = frequency;
            spec.format = format;
            spec.channels = static_cast<Uint8>(channels);
            shared_mixer = Mix_RegisterEffect(MIX_CHANNEL_POST, mix_callback, nullptr, this) != 0;
            if(shared_mixer) return true;
        }
        if(SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
        initialized = true;
        SDL_AudioSpec wanted{};
        wanted.freq = 48000;
        wanted.channels = 2;
        wanted.format = AUDIO_F32SYS;
        wanted.samples = 1024;
        wanted.callback = callback;
        wanted.userdata = this;
        device = SDL_OpenAudioDevice(nullptr, 0, &wanted, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
        return device != 0;
    }

    void pause(bool pause) {
        paused.store(pause);
        if(device) SDL_PauseAudioDevice(device, pause);
    }

    void clear() {
        std::lock_guard lock(mutex);
        blocks.clear();
        front_offset = 0;
        samples_queued = 0;
        consumed_frames.store(0);
    }

    void queue(std::vector<float> samples) {
        if(!valid() || samples.empty()) return;
        std::lock_guard lock(mutex);
        samples_queued += samples.size();
        blocks.push_back(std::move(samples));
    }

    double buffered() const {
        if(!valid()) return 0;
        std::lock_guard lock(mutex);
        return static_cast<double>(samples_queued) / (spec.freq * spec.channels);
    }

private:
    bool initialized = false;
    bool shared_mixer = false;
    std::atomic<bool> paused{true};
    mutable std::mutex mutex;
    std::deque<std::vector<float>> blocks;
    std::size_t front_offset = 0;
    std::size_t samples_queued = 0;

    void consume(void* bytes, int byte_count, bool additive) {
        if(!additive) std::memset(bytes, 0, byte_count);
        if(paused.load()) return;
        std::lock_guard lock(mutex);
        const std::size_t sample_bytes = spec.format == AUDIO_F32SYS ? sizeof(float) : sizeof(std::int16_t);
        std::size_t wanted = static_cast<std::size_t>(byte_count) / sample_bytes;
        std::size_t copied = 0;
        float volume = gain.load();
        while(copied < wanted && !blocks.empty()) {
            auto& block = blocks.front();
            auto count = std::min(wanted - copied, block.size() - front_offset);
            for(std::size_t i = 0; i < count; ++i) {
                float sample = block[front_offset + i] * volume;
                if(spec.format == AUDIO_F32SYS) {
                    auto* output = static_cast<float*>(bytes);
                    output[copied + i] = std::clamp((additive ? output[copied + i] : 0.0f) + sample, -1.0f, 1.0f);
                } else {
                    auto* output = static_cast<std::int16_t*>(bytes);
                    int mixed = (additive ? output[copied + i] : 0) + static_cast<int>(std::lrint(sample * 32767));
                    output[copied + i] = static_cast<std::int16_t>(std::clamp(mixed, -32768, 32767));
                }
            }
            copied += count;
            front_offset += count;
            samples_queued -= count;
            if(front_offset == block.size()) { blocks.pop_front(); front_offset = 0; }
        }
        // Silence inserted on underrun does not advance the media clock.
        consumed_frames.fetch_add(copied / spec.channels);
    }

    static void callback(void* userdata, Uint8* output_bytes, int byte_count) {
        static_cast<audio_sink*>(userdata)->consume(output_bytes, byte_count, false);
    }
    static void SDLCALL mix_callback(int, void* bytes, int byte_count, void* userdata) {
        static_cast<audio_sink*>(userdata)->consume(bytes, byte_count, true);
    }
};}

std::string clean_subtitle_text(const std::string& input, bool ass) {
    std::size_t offset = 0;
    if(ass) {
        for(int field = 0; field < 8; ++field) {
            auto comma = input.find(',', offset);
            if(comma == std::string::npos) { offset = 0; break; }
            offset = comma + 1;
        }
    }
    std::string output;
    bool markup = false;
    char end = 0;
    for(std::size_t i = offset; i < input.size(); ++i) {
        char ch = input[i];
        if(markup) { if(ch == end) markup = false; continue; }
        if(ch == '{' || ch == '<') { markup = true; end = ch == '{' ? '}' : '>'; continue; }
        if(ch == '\\' && i + 1 < input.size()) {
            char next = input[++i];
            if(next == 'N' || next == 'n') output += '\n';
            else if(next == 'h') output += ' ';
            else { output += '\\'; output += next; }
        } else if(ch != '\r') output += ch;
    }
    return output;
}

std::vector<subtitle_cue> read_subrip(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::vector<subtitle_cue> cues;
    std::string line;
    while(std::getline(input, line)) {
        auto arrow = line.find("-->");
        if(arrow == std::string::npos) continue;
        auto start = parse_subrip_time(line.substr(0, arrow));
        auto end = parse_subrip_time(line.substr(arrow + 3));
        std::string text, next;
        while(std::getline(input, next)) {
            if(!next.empty() && next.back() == '\r') next.pop_back();
            if(next.empty()) break;
            if(!text.empty()) text += '\n';
            text += next;
        }
        if(start >= 0 && end > start && !text.empty()) cues.push_back({start, end, clean_subtitle_text(text)});
    }
    std::stable_sort(cues.begin(), cues.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    return cues;
}

struct session::implementation {
    AVFormatContext* format = nullptr;
    AVCodecContext* video_codec = nullptr;
    AVCodecContext* audio_codec = nullptr;
    AVCodecContext* subtitle_codec = nullptr;
    AVBufferRef* hardware_device = nullptr;
    AVPixelFormat hardware_format = AV_PIX_FMT_NONE;
    SwsContext* scaler = nullptr;
    SwrContext* resampler = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* decoded = nullptr;
    AVFrame* transferred = nullptr;
    audio_sink audio;
    information metadata;
    options settings;
    state status = state::playing;
    std::string failure;
    int video_index = -1;
    int audio_index = -1;
    int subtitle_index = -1;
    double origin = 0;
    double anchor = 0;
    clock_type::time_point anchor_time = clock_type::now();
    double frozen_position = 0;
    double audio_anchor = 0;
    bool audio_anchored = false;
    bool eof = false;
    bool drained = false;
    bool loop = false;
    bool needs_frame = true;
    double speed = 1;
    double discard_before = 0;
    double last_video_pts = 0;
    double last_presented_pts = -1;
    bool resume_seek = false;
    std::deque<double> recent_frame_times;
    std::deque<frame> frames;
    std::optional<frame> published;
    std::vector<subtitle_cue> embedded_cues;
    std::vector<subtitle_cue> sidecar_cues;
    std::size_t max_frames = 4;

    implementation(const std::filesystem::path& path, options opts) : settings(std::move(opts)) {
        try {
            require(avformat_open_input(&format, path.string().c_str(), nullptr, nullptr), "Open video");
            require(avformat_find_stream_info(format, nullptr), "Read streams");
            origin = format->start_time == AV_NOPTS_VALUE ? 0.0 : static_cast<double>(format->start_time) / AV_TIME_BASE;
            metadata.duration = format->duration == AV_NOPTS_VALUE ? 0.0 : static_cast<double>(format->duration) / AV_TIME_BASE;
            video_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            require(video_index, "Find video stream");
            video_codec = open_codec(video_index, true);
            metadata.width = video_codec->width;
            metadata.height = video_codec->height;
            if(metadata.width <= 0 || metadata.height <= 0 || metadata.width > 16384 || metadata.height > 16384) throw std::runtime_error("Unsupported video dimensions");
            auto* stream = format->streams[video_index];
            auto rate = av_guess_frame_rate(format, stream, nullptr);
            if(rate.num > 0 && rate.den > 0) metadata.frame_rate = av_q2d(rate);
            auto sar = av_guess_sample_aspect_ratio(format, stream, nullptr);
            metadata.aspect_ratio = static_cast<double>(metadata.width) / metadata.height * (sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0);
            max_frames = std::clamp<std::size_t>(64u * 1024u * 1024u / (static_cast<std::size_t>(metadata.width) * metadata.height * 4u), 2, 12);
            for(unsigned int i = 0; i < format->nb_streams; ++i) {
                auto* candidate = format->streams[i];
                if(!avcodec_find_decoder(candidate->codecpar->codec_id)) continue;
                if(candidate->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) metadata.audio_tracks.push_back({static_cast<int>(i), track_label(candidate, "Audio")});
                if(candidate->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                    auto* descriptor = avcodec_descriptor_get(candidate->codecpar->codec_id);
                    if(descriptor && (descriptor->props & AV_CODEC_PROP_TEXT_SUB)) metadata.subtitle_tracks.push_back({static_cast<int>(i), track_label(candidate, "Subtitles")});
                }
            }
            if(settings.audio_output && !metadata.audio_tracks.empty()) {
                if(audio.open()) {
                    audio_index = metadata.audio_tracks.front().index;
                    audio_codec = open_codec(audio_index, false);
                    metadata.audio_status = "Audio output active";
                } else metadata.audio_status = std::string("Audio unavailable: ") + SDL_GetError();
            } else metadata.audio_status = metadata.audio_tracks.empty() ? "No audio track" : "Audio output disabled";
            auto sidecar = path;
            sidecar.replace_extension(".srt");
            sidecar_cues = read_subrip(sidecar);
            if(!sidecar_cues.empty()) metadata.subtitle_tracks.push_back({-2, "Sidecar subtitles"});
            packet = av_packet_alloc();
            decoded = av_frame_alloc();
            transferred = av_frame_alloc();
            if(!packet || !decoded || !transferred) throw std::bad_alloc();
            anchor_time = clock_type::now();
        } catch(...) { release(); throw; }
    }

    ~implementation() { release(); }

    void release() {
        audio.pause(true);
        av_packet_free(&packet);
        av_frame_free(&decoded);
        av_frame_free(&transferred);
        avcodec_free_context(&video_codec);
        avcodec_free_context(&audio_codec);
        avcodec_free_context(&subtitle_codec);
        avformat_close_input(&format);
        av_buffer_unref(&hardware_device);
        sws_freeContext(scaler);
        scaler = nullptr;
        swr_free(&resampler);
    }

    static AVPixelFormat choose_format(AVCodecContext* context, const AVPixelFormat* formats) {
        auto* self = static_cast<implementation*>(context->opaque);
        for(auto* current = formats; *current != AV_PIX_FMT_NONE; ++current) if(*current == self->hardware_format) return *current;
        for(auto* current = formats; *current != AV_PIX_FMT_NONE; ++current) {
            const auto* desc = av_pix_fmt_desc_get(*current);
            if(desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
                self->metadata.decoder = std::string(context->codec->name) + " · software";
                return *current;
            }
        }
        return AV_PIX_FMT_NONE;
    }

    AVCodecContext* open_codec(int index, bool allow_hardware) {
        auto* stream = format->streams[index];
        auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if(!decoder) throw std::runtime_error("No decoder for selected stream");
        auto* context = avcodec_alloc_context3(decoder);
        if(!context) throw std::bad_alloc();
        try {
            require(avcodec_parameters_to_context(context, stream->codecpar), "Set decoder parameters");
            context->pkt_timebase = stream->time_base;
            context->thread_count = 2;
            metadata.decoder = allow_hardware ? std::string(decoder->name) + " · software" : metadata.decoder;
            if(allow_hardware && settings.hardware_acceleration != "off") {
                for(int i = 0; const auto* config = avcodec_get_hw_config(decoder, i); ++i) {
                    if(!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) continue;
                    std::string name = av_hwdevice_get_type_name(config->device_type);
                    std::string preferred = settings.hardware_acceleration;
                    if(preferred == "d3d11") preferred = "d3d11va";
                    if(preferred != "auto" && preferred != name) continue;
                    if(av_hwdevice_ctx_create(&hardware_device, config->device_type, nullptr, nullptr, 0) < 0) continue;
                    hardware_format = config->pix_fmt;
                    context->hw_device_ctx = av_buffer_ref(hardware_device);
                    context->opaque = this;
                    context->get_format = choose_format;
                    metadata.decoder = std::string(decoder->name) + " · " + name;
                    break;
                }
            }
            int opened = avcodec_open2(context, decoder, nullptr);
            if(opened < 0 && context->hw_device_ctx) {
                avcodec_free_context(&context);
                av_buffer_unref(&hardware_device);
                hardware_format = AV_PIX_FMT_NONE;
                context = avcodec_alloc_context3(decoder);
                if(!context) throw std::bad_alloc();
                require(avcodec_parameters_to_context(context, stream->codecpar), "Set software decoder parameters");
                context->pkt_timebase = stream->time_base;
                context->thread_count = 2;
                metadata.decoder = std::string(decoder->name) + " · software";
                opened = avcodec_open2(context, decoder, nullptr);
            }
            require(opened, "Open decoder");
            return context;
        } catch(...) { avcodec_free_context(&context); throw; }
    }

    double current_position() const {
        if(status != state::playing) return frozen_position;
        if(audio.valid() && audio_anchored && speed == 1.0 && !(eof && audio.buffered() == 0)) {
            return audio_anchor + static_cast<double>(audio.consumed_frames.load()) / audio.spec.freq;
        }
        return anchor + std::chrono::duration<double>(clock_type::now() - anchor_time).count() * speed;
    }

    void set_clock(double position) {
        anchor = position;
        frozen_position = position;
        anchor_time = clock_type::now();
    }

    double timestamp(AVFrame* frame, int index, double fallback) const {
        auto pts = frame->best_effort_timestamp;
        if(pts == AV_NOPTS_VALUE) pts = frame->pts;
        return pts == AV_NOPTS_VALUE ? fallback : static_cast<double>(pts) * av_q2d(format->streams[index]->time_base) - origin;
    }

    void video_frame(AVFrame* frame) {
        double pts = timestamp(frame, video_index, last_video_pts);
        last_video_pts = pts + 1.0 / metadata.frame_rate;
        recent_frame_times.push_back(std::max(0.0, pts));
        if(recent_frame_times.size() > 512) recent_frame_times.pop_front();
        if(pts + 0.000001 < discard_before) return;
        AVFrame* source = frame;
        auto* descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
        if(descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            av_frame_unref(transferred);
            require(av_hwframe_transfer_data(transferred, frame, 0), "Download decoded hardware frame");
            source = transferred;
        }
        if(source->width <= 0 || source->height <= 0 || source->width > 16384 || source->height > 16384) throw std::runtime_error("Unsupported decoded dimensions");
        scaler = sws_getCachedContext(scaler, source->width, source->height, static_cast<AVPixelFormat>(source->format), source->width, source->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if(!scaler) throw std::runtime_error("Cannot create video colour converter");
        // Respect limited/full range and stream colour matrix instead of using
        // one hard-coded YUV420 matrix for every source.
        auto colorspace = frame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : frame->colorspace == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020 : SWS_CS_ITU601;
        auto* coefficients = sws_getCoefficients(colorspace);
        sws_setColorspaceDetails(scaler, coefficients, frame->color_range == AVCOL_RANGE_JPEG, coefficients, 1, 0, 1 << 16, 1 << 16);
        struct frame output;
        output.width = source->width;
        output.height = source->height;
        output.timestamp = std::max(0.0, pts);
        output.aspect_ratio = metadata.aspect_ratio;
        auto bytes = static_cast<std::size_t>(source->width) * source->height * 4u;
        output.rgba.resize(bytes);
        uint8_t* destination[] = {output.rgba.data(), nullptr, nullptr, nullptr};
        int stride[] = {source->width * 4, 0, 0, 0};
        require(sws_scale(scaler, source->data, source->linesize, 0, source->height, destination, stride), "Convert video frame");
        frames.push_back(std::move(output));
    }

    void audio_frame(AVFrame* frame) {
        if(!audio.valid() || speed != 1.0) return;
        if(!resampler) {
            AVChannelLayout target{};
            av_channel_layout_default(&target, audio.spec.channels);
            int configured = swr_alloc_set_opts2(&resampler, &target, AV_SAMPLE_FMT_FLT, audio.spec.freq, &frame->ch_layout, static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&target);
            require(configured, "Create audio resampler");
            require(swr_init(resampler), "Initialize audio resampler");
        }
        auto delay = swr_get_delay(resampler, frame->sample_rate);
        int capacity = static_cast<int>(av_rescale_rnd(delay + frame->nb_samples, audio.spec.freq, frame->sample_rate, AV_ROUND_UP));
        if(capacity <= 0) return;
        std::vector<float> pcm(static_cast<std::size_t>(capacity) * audio.spec.channels);
        uint8_t* output[] = {reinterpret_cast<uint8_t*>(pcm.data())};
        int samples = swr_convert(resampler, output, capacity, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
        require(samples, "Resample audio");
        double pts = timestamp(frame, audio_index, current_position()) - static_cast<double>(delay) / frame->sample_rate;
        auto skip = static_cast<int>(std::clamp(std::ceil((discard_before - pts) * audio.spec.freq), 0.0, static_cast<double>(samples)));
        if(skip == samples) return;
        pcm.resize(static_cast<std::size_t>(samples) * audio.spec.channels);
        pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(skip * audio.spec.channels));
        if(!audio_anchored) {
            audio_anchor = std::max(discard_before, pts + static_cast<double>(skip) / audio.spec.freq);
            audio_anchored = true;
        }
        audio.queue(std::move(pcm));
    }

    void decode(AVCodecContext* codec, AVPacket* input, bool video) {
        if(!codec) return;
        auto receive = [&] {
            while(true) {
                int result = avcodec_receive_frame(codec, decoded);
                if(result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
                require(result, "Decode media frame");
                if(video) video_frame(decoded); else audio_frame(decoded);
                av_frame_unref(decoded);
            }
        };
        int sent = avcodec_send_packet(codec, input);
        if(sent == AVERROR(EAGAIN)) { receive(); sent = avcodec_send_packet(codec, input); }
        if(sent != AVERROR_EOF) require(sent, "Send decoder packet");
        receive();
    }

    void drain_audio_resampler() {
        if(!resampler || !audio.valid() || speed != 1.0) return;
        while(true) {
            std::vector<float> pcm(static_cast<std::size_t>(256 * audio.spec.channels));
            uint8_t* output[] = {reinterpret_cast<uint8_t*>(pcm.data())};
            int samples = swr_convert(resampler, output, 256, nullptr, 0);
            require(samples, "Drain audio resampler");
            if(samples == 0) break;
            pcm.resize(static_cast<std::size_t>(samples * audio.spec.channels));
            audio.queue(std::move(pcm));
        }
    }

    void decode_subtitle(AVPacket* input) {
        if(!subtitle_codec) return;
        AVSubtitle subtitle{};
        int got = 0;
        int result = avcodec_decode_subtitle2(subtitle_codec, &subtitle, &got, input);
        if(result < 0 || !got) { avsubtitle_free(&subtitle); return; }
        double pts = subtitle.pts != AV_NOPTS_VALUE ? static_cast<double>(subtitle.pts) / AV_TIME_BASE - origin : input->pts != AV_NOPTS_VALUE ? static_cast<double>(input->pts) * av_q2d(format->streams[subtitle_index]->time_base) - origin : current_position();
        double start = pts + subtitle.start_display_time / 1000.0;
        double end = pts + subtitle.end_display_time / 1000.0;
        if(end <= start && input->duration > 0) end = start + input->duration * av_q2d(format->streams[subtitle_index]->time_base);
        if(end <= start) end = start + 4;
        std::string text;
        for(unsigned int i = 0; i < subtitle.num_rects; ++i) {
            auto* rect = subtitle.rects[i];
            std::string part = rect->text ? clean_subtitle_text(rect->text) : rect->ass ? clean_subtitle_text(rect->ass, true) : "";
            if(!part.empty()) { if(!text.empty()) text += '\n'; text += part; }
        }
        if(!text.empty() && end >= discard_before) embedded_cues.push_back({start, end, std::move(text)});
        avsubtitle_free(&subtitle);
    }

    bool seek_to(double seconds) {
        double upper = metadata.duration > 0 ? std::max(0.0, metadata.duration - 0.001) : std::numeric_limits<double>::max();
        seconds = std::clamp(seconds, 0.0, upper);
        // Include a text-cue preroll: subtitles often start before the nearest
        // video keyframe and remain active across the requested seek position.
        double demux_target = subtitle_index >= 0 ? std::max(0.0, seconds - 30.0) : seconds;
        auto target = static_cast<int64_t>((demux_target + origin) * AV_TIME_BASE);
        int result = avformat_seek_file(format, -1, std::numeric_limits<int64_t>::min(), target, target, AVSEEK_FLAG_BACKWARD);
        if(result < 0) { failure = "Seek failed: " + av_error(result); return false; }
        avcodec_flush_buffers(video_codec);
        if(audio_codec) avcodec_flush_buffers(audio_codec);
        if(subtitle_codec) avcodec_flush_buffers(subtitle_codec);
        swr_free(&resampler);
        audio.clear();
        audio_anchored = false;
        frames.clear();
        published.reset();
        embedded_cues.clear();
        discard_before = seconds;
        last_video_pts = seconds;
        eof = false;
        drained = false;
        needs_frame = true;
        failure.clear();
        set_clock(seconds);
        if(status == state::ended) status = state::paused;
        return true;
    }

    void pump() {
        if(status == state::failed || status == state::ended) return;
        if(status == state::paused && !needs_frame) return;
        try {
            auto deadline = clock_type::now() + std::chrono::milliseconds(12);
            for(int count = 0; count < 128 && !eof; ++count) {
                if(frames.size() >= max_frames) break;
                if(audio.buffered() >= 1.0) break;
                bool video_ready = !frames.empty() && frames.back().timestamp > current_position() + 0.25;
                bool audio_ready = !audio_codec || speed != 1.0 || audio.buffered() >= 0.25;
                if(video_ready && audio_ready) break;
                if(count > 0 && clock_type::now() >= deadline) break;
                int read = av_read_frame(format, packet);
                if(read == AVERROR_EOF) { eof = true; break; }
                require(read, "Read media packet");
                if(packet->stream_index == video_index) decode(video_codec, packet, true);
                else if(packet->stream_index == audio_index) decode(audio_codec, packet, false);
                else if(packet->stream_index == subtitle_index) decode_subtitle(packet);
                av_packet_unref(packet);
            }
            if(eof && !drained) {
                decode(video_codec, nullptr, true);
                decode(audio_codec, nullptr, false);
                drain_audio_resampler();
                drained = true;
            }
            double now = current_position();
            if(audio.valid() && audio_anchored && audio.buffered() > 0 && status == state::playing && speed == 1.0) {
                // Keep the wall-clock fallback aligned for trailing silent video.
                set_clock(now);
                audio.pause(false);
            }
            while(!frames.empty() && (frames.front().timestamp <= now + 0.005 || (needs_frame && status == state::paused))) {
                published = std::move(frames.front());
                frames.pop_front();
                last_presented_pts = published->timestamp;
                needs_frame = false;
                if(status == state::paused) { set_clock(published->timestamp); break; }
            }
            std::erase_if(embedded_cues, [now](const auto& cue) { return cue.end < now - 5.0; });
            if(eof && drained && frames.empty() && audio.buffered() < 0.001 && now >= last_video_pts - 0.02) {
                if(loop) { seek_to(0); status = state::playing; }
                else { set_clock(std::max(0.0, last_video_pts)); status = state::ended; }
            }
        } catch(const std::exception& exception) {
            av_packet_unref(packet);
            failure = exception.what();
            set_clock(current_position());
            status = state::failed;
            audio.pause(true);
        }
    }
};

session::session(const std::filesystem::path& path, options settings) : impl(std::make_unique<implementation>(path, std::move(settings))) {}
session::~session() = default;
const information& session::info() const { return impl->metadata; }
state session::playback_state() const { return impl->status; }
const std::string& session::error() const { return impl->failure; }
double session::position() const { return std::max(0.0, impl->current_position()); }
double session::buffered_audio_seconds() const { return impl->audio.buffered(); }
std::size_t session::played_audio_frames() const { return impl->audio.consumed_frames.load(); }
void session::pump() { impl->pump(); }
std::optional<frame> session::take_frame() { return std::exchange(impl->published, std::nullopt); }
bool session::seek(double seconds) { return std::isfinite(seconds) && impl->seek_to(seconds); }

void session::pause(bool paused) {
    if(impl->status == state::failed) return;
    if(impl->status == state::ended && !paused) impl->seek_to(0);
    auto now = position();
    if(!paused && impl->resume_seek) {
        impl->seek_to(now);
        impl->resume_seek = false;
    }
    impl->set_clock(now);
    impl->status = paused ? state::paused : state::playing;
    impl->audio.pause(paused || impl->speed != 1.0);
}

void session::step(int direction) {
    pause(true);
    double displayed = impl->last_presented_pts >= 0 ? impl->last_presented_pts : position();
    double target = displayed + 0.00001;
    if(direction < 0) {
        target = std::max(0.0, displayed - 1.0 / impl->metadata.frame_rate);
        double closest = -1;
        for(double pts : impl->recent_frame_times) if(pts < displayed - 0.00001) closest = std::max(closest, pts);
        if(closest >= 0) target = closest;
    }
    impl->seek_to(target);
    impl->resume_seek = true;
}
void session::replay(double seconds) { seek(position() - std::max(0.0, seconds)); }
void session::set_repeat(bool enabled) { impl->loop = enabled; }
bool session::repeat() const { return impl->loop; }
void session::set_volume(float volume) { impl->audio.gain.store(std::clamp(volume, 0.0f, 1.0f)); }
float session::volume() const { return impl->audio.gain.load(); }
void session::set_rate(double rate) {
    if(!std::isfinite(rate)) return;
    auto position = this->position();
    impl->speed = std::clamp(rate, 0.25, 2.0);
    impl->seek_to(position);
    impl->audio.pause(impl->speed != 1.0 || impl->status != state::playing);
}
double session::rate() const { return impl->speed; }

bool session::select_audio(int stream_index) {
    if(!impl->audio.valid()) return false;
    if(std::none_of(impl->metadata.audio_tracks.begin(), impl->metadata.audio_tracks.end(), [=](const auto& t) { return t.index == stream_index; })) return false;
    try {
        auto* next = impl->open_codec(stream_index, false);
        avcodec_free_context(&impl->audio_codec);
        impl->audio_codec = next;
        impl->audio_index = stream_index;
        return seek(position());
    } catch(const std::exception& exception) { impl->failure = exception.what(); return false; }
}

bool session::select_subtitle(int stream_index) {
    if(stream_index != -1 && std::none_of(impl->metadata.subtitle_tracks.begin(), impl->metadata.subtitle_tracks.end(), [=](const auto& t) { return t.index == stream_index; })) return false;
    try {
        auto* next = stream_index >= 0 ? impl->open_codec(stream_index, false) : nullptr;
        avcodec_free_context(&impl->subtitle_codec);
        impl->subtitle_codec = next;
        impl->subtitle_index = stream_index;
        return seek(position());
    } catch(const std::exception& exception) { impl->failure = exception.what(); return false; }
}
int session::selected_audio() const { return impl->audio_index; }
int session::selected_subtitle() const { return impl->subtitle_index; }
std::string session::subtitle_text() const {
    if(impl->subtitle_index == -1) return {};
    const auto& cues = impl->subtitle_index == -2 ? impl->sidecar_cues : impl->embedded_cues;
    double now = position();
    std::string result;
    for(const auto& cue : cues) if(cue.start <= now && now < cue.end) {
        if(!result.empty()) result += '\n';
        result += cue.text;
    }
    return result;
}

}
