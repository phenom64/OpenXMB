#include <openxmb/video.hpp>
#include <openxmb/video_worker.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

namespace video = openxmb::video;
using namespace std::chrono_literals;

std::atomic<int> peak_sample{0};
void SDLCALL measure_mix(void*, Uint8* bytes, int length) {
    auto* samples = reinterpret_cast<std::int16_t*>(bytes);
    int peak = 0;
    for(int i = 0; i < length / static_cast<int>(sizeof(std::int16_t)); ++i) peak = std::max(peak, std::abs(static_cast<int>(samples[i])));
    peak_sample.store(std::max(peak_sample.load(), peak));
}

void check(bool condition, const char* description) {
    if(!condition) throw std::runtime_error(description);
}

template<class Callback>
void run_for(video::session& player, std::chrono::milliseconds duration, Callback callback) {
    auto end = std::chrono::steady_clock::now() + duration;
    do {
        player.pump();
        if(player.playback_state() == video::state::failed) throw std::runtime_error(player.error());
        if(auto frame = player.take_frame()) callback(*frame);
        std::this_thread::sleep_for(5ms);
    } while(std::chrono::steady_clock::now() < end);
}

void run_for(video::session& player, std::chrono::milliseconds duration) {
    run_for(player, duration, [](const auto&) {});
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "fixture path is required");
        std::filesystem::path fixture = argv[1];
        auto sidecar = fixture;
        sidecar.replace_extension(".srt");
        const auto cues = video::read_subrip(sidecar);
        check(cues.size() == 2, "SubRip parser did not retain both cues");
        check(std::abs(cues[0].start - 0.15) < 0.001 && std::abs(cues[0].end - 0.75) < 0.001, "SubRip timestamps changed");
        check(cues[0].text == "First line", "SubRip markup was not removed");
        check(video::clean_subtitle_text("0,0,Default,,0,0,0,,{\\b1}Hello\\Nworld", true) == "Hello\nworld", "ASS text conversion failed");
        bool rejected = false;
        try { video::session missing(fixture.parent_path() / "missing-video.mkv", {.hardware_acceleration = "off", .audio_output = false}); }
        catch(const std::exception&) { rejected = true; }
        check(rejected, "Missing file did not report an open failure");

        {
            video::session player(fixture, {.hardware_acceleration = "off"});
            check(player.info().width == 96 && player.info().height == 64, "Fixture dimensions did not decode");
            check(player.info().duration > 1.9 && player.info().duration < 2.2, "Fixture duration was not read");
            check(player.info().audio_tracks.size() == 2, "Audio streams were not enumerated");
            check(player.info().subtitle_tracks.size() >= 2, "Embedded and sidecar subtitles were not enumerated");
            check(player.selected_audio() >= 0, "SDL dummy audio device did not open");
            check(player.select_subtitle(player.info().subtitle_tracks.front().index), "Embedded subtitle selection failed");
            unsigned int frames = 0;
            double previous_pts = -1;
            bool colored = false, subtitle_seen = false;
            auto end = std::chrono::steady_clock::now() + 550ms;
            while(std::chrono::steady_clock::now() < end) {
                player.pump();
                check(player.playback_state() != video::state::failed, player.error().c_str());
                if(auto frame = player.take_frame()) {
                    check(frame->rgba.size() == 96 * 64 * 4, "Decoded YUV frame did not produce packed RGBA");
                    check(frame->timestamp >= previous_pts, "Presentation timestamps moved backwards");
                    check(frame->timestamp <= player.position() + 0.12, "Frame was presented before its timestamp");
                    previous_pts = frame->timestamp;
                    for(std::size_t i = 0; i < frame->rgba.size(); i += 4) {
                        if(frame->rgba[i] != frame->rgba[i + 1] || frame->rgba[i + 1] != frame->rgba[i + 2]) colored = true;
                    }
                    ++frames;
                }
                subtitle_seen = subtitle_seen || player.subtitle_text() == "First line";
                std::this_thread::sleep_for(5ms);
            }
            check(frames >= 4 && frames <= 10, "Playback was not paced to fixture timestamps");
            check(colored, "YUV colour conversion produced no coloured pixels");
            check(subtitle_seen, "Embedded subtitle did not appear during its time interval");
            check(player.played_audio_frames() > 1000, "Audio callback did not consume decoded PCM");
            check(player.position() > 0.3 && player.position() < 0.8, "Audio master clock did not advance normally");

            player.pause(true);
            double paused_at = player.position();
            auto audio_at = player.played_audio_frames();
            run_for(player, 100ms);
            check(std::abs(player.position() - paused_at) < 0.001, "Paused presentation clock advanced");
            check(player.played_audio_frames() == audio_at, "Paused audio continued to play");
            player.set_volume(0.35f);
            check(std::abs(player.volume() - 0.35f) < 0.001, "Audio volume was not retained");
            player.set_volume(4);
            check(player.volume() == 1, "Audio volume exceeded its upper bound");
            check(player.seek(1.1), "Accurate seek failed");
            bool seek_frame = false;
            run_for(player, 80ms, [&](const auto& frame) {
                check(frame.timestamp >= 1.08 && frame.timestamp < 1.25, "Seek presented a stale frame");
                seek_frame = true;
            });
            check(seek_frame, "Paused seek did not display its target frame");
            check(player.subtitle_text() == "Second line", "Subtitle clock did not follow seek");
            check(player.select_audio(player.info().audio_tracks.back().index), "Alternate audio track selection failed");
            check(player.selected_audio() == player.info().audio_tracks.back().index, "Alternate audio track was not selected");
            check(player.select_subtitle(-2), "Sidecar selection failed");
            run_for(player, 50ms);
            check(player.subtitle_text() == "Second line", "Sidecar subtitles were not timed to playback");
            check(player.select_subtitle(-1) && player.subtitle_text().empty(), "Turning subtitles off did not clear text");
            check(!player.select_audio(9999) && !player.select_subtitle(9999), "Invalid track index was accepted");

            auto before_step = player.position();
            player.step(1);
            run_for(player, 80ms);
            check(player.playback_state() == video::state::paused && player.position() > before_step, "Forward frame stepping did not advance while paused");
            before_step = player.position();
            player.step(-1);
            run_for(player, 80ms);
            check(player.position() < before_step, "Backward frame stepping did not move to the previous frame");

            player.set_rate(2);
            player.pause(false);
            double fast_start = player.position();
            run_for(player, 120ms);
            check(player.position() - fast_start > 0.17, "Speed playback did not use the scaled presentation clock");
            player.set_rate(1);
            check(player.seek(1.75), "End-of-file seek failed");
            player.pause(false);
            run_for(player, 600ms);
            check(player.playback_state() == video::state::ended, "Decoder did not drain and reach end of file");
            player.set_repeat(true);
            check(player.seek(1.7), "Repeat seek failed");
            player.pause(false);
            run_for(player, 650ms);
            check(player.playback_state() == video::state::playing && player.position() < 1.0, "Repeat did not restart the media clock and decoder");
        }
        {
            video::session silent(fixture, {.hardware_acceleration = "off", .audio_output = false});
            unsigned int frames = 0;
            run_for(silent, 220ms, [&](const auto&) { ++frames; });
            check(silent.position() > 0.18 && frames >= 2, "Video without audio output did not use the wall clock");
            check(silent.seek(-100), "Seek to start failed");
            check(silent.position() < 0.05, "Negative seek did not clamp to start");
            check(!silent.seek(std::numeric_limits<double>::quiet_NaN()), "Non-finite seek was accepted");
        }
        check(SDL_InitSubSystem(SDL_INIT_AUDIO) == 0, "Mixer fixture could not initialize SDL audio");
        check(Mix_OpenAudio(48000, AUDIO_S16SYS, 2, 1024) == 0, "Mixer fixture could not open the dummy output");
        Mix_SetPostMix(measure_mix, nullptr);
        {
            video::session mixed(fixture, {.hardware_acceleration = "off"});
            check(mixed.selected_audio() >= 0, "Video could not share the shell audio mixer");
            run_for(mixed, 220ms);
            check(mixed.played_audio_frames() > 1000 && peak_sample.load() > 100, "Shared mixer output did not contain decoded video audio");
            mixed.set_volume(0);
            std::this_thread::sleep_for(40ms);
            peak_sample.store(0);
            run_for(mixed, 100ms);
            check(peak_sample.load() == 0, "Volume zero did not silence the shared video output");
        }
        Mix_SetPostMix(nullptr, nullptr);
        Mix_CloseAudio();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        {
            video::worker asynchronous;
            auto posted = std::chrono::steady_clock::now();
            asynchronous.set_path(fixture.parent_path() / "missing-video.mkv", {.hardware_acceleration = "off", .audio_output = false});
            asynchronous.set_path(fixture, {.hardware_acceleration = "off", .audio_output = false});
            asynchronous.set_repeat(true);
            check(std::chrono::steady_clock::now() - posted < 100ms, "Posting a background video request blocked the UI");
            auto deadline = std::chrono::steady_clock::now() + 2s;
            bool picture = false;
            while(std::chrono::steady_clock::now() < deadline) {
                if(auto decoded = asynchronous.take_frame()) { picture = decoded->rgba.size() == 96 * 64 * 4; break; }
                std::this_thread::sleep_for(5ms);
            }
            check(picture && asynchronous.snapshot()->ready, "Persistent video worker did not publish the replacement file");
            check(asynchronous.snapshot()->error.empty(), "Superseded file-open failure overwrote the active worker state");
            asynchronous.pause(true);
            std::this_thread::sleep_for(50ms);
            double at_pause = asynchronous.snapshot()->position;
            std::this_thread::sleep_for(70ms);
            check(std::abs(asynchronous.snapshot()->position - at_pause) < 0.001, "Hidden background video did not pause its decode clock");
            asynchronous.seek(1.0);
            deadline = std::chrono::steady_clock::now() + 2s;
            bool target = false;
            while(std::chrono::steady_clock::now() < deadline) {
                if(auto decoded = asynchronous.take_frame(); decoded && decoded->timestamp >= 0.99) { target = true; break; }
                std::this_thread::sleep_for(5ms);
            }
            check(target, "Paused asynchronous seek did not publish a preview frame");
            asynchronous.set_path({}, {.hardware_acceleration = "off", .audio_output = false});
            std::this_thread::sleep_for(50ms);
            check(!asynchronous.snapshot()->ready && !asynchronous.take_frame(), "Clearing a video path retained a stale presentation frame");
        }
        std::cout << "Video playback passed: RGBA conversion, PCM output, shared mixer volume, A/V pacing, pause, seek, tracks, subtitles, frame stepping, speed, EOF, repeat and asynchronous background state.\n";
        return 0;
    } catch(const std::exception& exception) {
        std::cerr << "Video playback test failed: " << exception.what() << '\n';
        return 1;
    }
}
