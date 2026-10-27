#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <openxmb/video.hpp>
extern "C" {
#include <libavformat/avformat.h>
}
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

void require(int result, const char *what) {
    if (result < 0)
        throw std::runtime_error(what);
}
// Keep valid codec payloads/timestamps, deliberately mux a legal but badly
// interleaved video burst between the initial audio block and remaining audio.
void remux(const char *source, const char *destination) {
    AVFormatContext *input = nullptr;
    AVFormatContext *output = nullptr;
    std::vector<AVPacket *> prefix, video, tail;
    auto cleanup = [&] {
        for (auto *list : {&prefix, &video, &tail})
            for (auto *&p : *list)
                av_packet_free(&p);
        if (output) {
            if (output->pb)
                avio_closep(&output->pb);
            avformat_free_context(output);
        }
        avformat_close_input(&input);
    };
    try {
        require(avformat_open_input(&input, source, nullptr, nullptr), "open fixture");
        require(avformat_find_stream_info(input, nullptr), "read fixture");
        require(avformat_alloc_output_context2(&output, nullptr, "matroska", destination), "allocate remux");
        for (unsigned i = 0; i < input->nb_streams; ++i) {
            auto *stream = avformat_new_stream(output, nullptr);
            if (!stream)
                throw std::bad_alloc();
            require(avcodec_parameters_copy(stream->codecpar, input->streams[i]->codecpar), "copy stream");
            stream->time_base = input->streams[i]->time_base;
        }
        require(avio_open(&output->pb, destination, AVIO_FLAG_WRITE), "open remux");
        require(avformat_write_header(output, nullptr), "write header");
        AVPacket *packet = av_packet_alloc();
        if (!packet)
            throw std::bad_alloc();
        while (av_read_frame(input, packet) >= 0) {
            auto *stream = input->streams[packet->stream_index];
            const double pts = packet->pts * av_q2d(stream->time_base);
            auto &list = stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? video : pts < 0.25 ? prefix : tail;
            list.push_back(packet);
            packet = av_packet_alloc();
            if (!packet)
                throw std::bad_alloc();
        }
        av_packet_free(&packet);
        for (auto *list : {&prefix, &video, &tail})
            for (auto *p : *list) {
                av_packet_rescale_ts(p, input->streams[p->stream_index]->time_base,
                                     output->streams[p->stream_index]->time_base);
                require(av_write_frame(output, p), "write noninterleaved packet");
            }
        require(av_write_trailer(output), "write trailer");
        cleanup();
    } catch (...) {
        cleanup();
        throw;
    }
}
int main(int argc, char **argv) {
    SDL_SetMainReady();
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    try {
        if (argc == 4 && std::string(argv[1]) == "--remux") {
            remux(argv[2], argv[3]);
            return 0;
        }
        if (argc < 2)
            throw std::runtime_error("fixture argument required");
        SDL_InitSubSystem(SDL_INIT_AUDIO);
        require(Mix_OpenAudio(48000, AUDIO_S16SYS, 2, 512), "open dummy mixer");
        if (argc == 3 && std::string(argv[1]) == "--limit") {
            {
                openxmb::video::session player(argv[2], {.hardware_acceleration = "off", .native_yuv = true});
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (player.playback_state() != openxmb::video::state::failed &&
                       std::chrono::steady_clock::now() < deadline) {
                    player.pump();
                    player.take_frame();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (player.playback_state() != openxmb::video::state::failed ||
                    player.error().find("bounded packet read-ahead") == std::string::npos)
                    throw std::runtime_error("pathological packet interleave was not bounded");
                player.pump(); // Terminal failure remains safe and does not allocate more.
            }
            Mix_CloseAudio();
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            std::cout << "Bounded packet read-ahead limit passed\n";
            return 0;
        }
        {
            openxmb::video::session player(argv[1], {.hardware_acceleration = "off", .native_yuv = true});
            const auto start = std::chrono::steady_clock::now();
            double last_pts = -1;
            unsigned frames = 0;
            double report = 0;
            while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
                player.pump();
                if (player.playback_state() == openxmb::video::state::failed)
                    throw std::runtime_error(player.error());
                if (auto frame = player.take_frame()) {
                    if (frame->timestamp + 0.001 < last_pts)
                        throw std::runtime_error("frame timestamps regressed");
                    last_pts = frame->timestamp;
                    ++frames;
                }
                const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                if (wall > report) {
                    std::cout << "wall=" << wall << " pos=" << player.position()
                              << " buffered=" << player.buffered_audio_seconds()
                              << " played=" << player.played_audio_frames() << " frame=" << last_pts << std::endl;
                    report += 0.5;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (player.position() < 2.0 || player.played_audio_frames() < 48000 || last_pts < 1.9 || frames < 15)
                throw std::runtime_error("audio/video stalled behind full future-video queue");
            player.pause(true);
            const double position = player.position();
            const auto audio = player.played_audio_frames();
            for (int i = 0; i < 30; ++i) {
                player.pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (player.position() != position || player.played_audio_frames() != audio)
                throw std::runtime_error("paused lookahead playback advanced");
            if (!player.seek(1))
                throw std::runtime_error("seek failed");
            bool preview = false;
            for (int i = 0; i < 100; ++i) {
                player.pump();
                if (auto f = player.take_frame()) {
                    if (f->timestamp < 0.999 || f->timestamp > 1.1)
                        throw std::runtime_error("deferred packets leaked across seek");
                    preview = true;
                    break;
                }
            }
            if (!preview)
                throw std::runtime_error("paused seek preview missing");
            player.pause(false);
            last_pts = 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
            while (player.playback_state() == openxmb::video::state::playing &&
                   std::chrono::steady_clock::now() < deadline) {
                player.pump();
                if (auto f = player.take_frame()) {
                    if (f->timestamp + 0.001 < last_pts)
                        throw std::runtime_error("deferred EOF timestamps regressed");
                    last_pts = f->timestamp;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (player.playback_state() != openxmb::video::state::ended || last_pts < 5.9)
                throw std::runtime_error("deferred video was not completely drained at EOF");
        }
        Mix_CloseAudio();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        std::cout << "Interleaved audio/video progress and seek isolation passed\n";
    } catch (const std::exception &e) {
        std::cerr << argv[argc - 1] << ": " << e.what() << '\n';
        return 1;
    }
}
