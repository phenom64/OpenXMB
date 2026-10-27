// Optional local-file hardware/software throughput probe. No GPU renderer required.
// Usage: probe file [native|rgba] [seconds] [mixer|device] [auto|off|dxva2|d3d11va] [pump_delay_ms]
// [--require-realtime]
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <openxmb/video.hpp>
#include <thread>
int main(int argc, char **argv) {
    SDL_SetMainReady();
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (argc < 2)
        return 2;
    const bool native = argc < 3 || std::string(argv[2]) != "rgba";
    const double duration = argc > 3 ? std::stod(argv[3]) : 12;
    const bool mixer = argc > 4 && std::string(argv[4]) == "mixer";
    const std::string acceleration = argc > 5 ? argv[5] : "off";
    const unsigned delay = argc > 6 ? std::stoul(argv[6]) : 5;
    if (mixer) {
        SDL_InitSubSystem(SDL_INIT_AUDIO);
        if (Mix_OpenAudio(48000, AUDIO_S16SYS, 2, 1024) != 0)
            return 3;
    }
    try {
        openxmb::video::session player(argv[1], {.hardware_acceleration = acceleration, .native_yuv = native});
        std::cout << player.info().width << 'x' << player.info().height << " fps=" << player.info().frame_rate
                  << " decoder=" << player.info().decoder << " audio=" << player.info().audio_status
                  << " native=" << native << std::endl;
        auto start = std::chrono::steady_clock::now();
        double report = 0, last_pts = -1, max_pump = 0, sum_pump = 0;
        unsigned frames = 0, pumps = 0;
        while (true) {
            const auto now = std::chrono::steady_clock::now();
            const double wall = std::chrono::duration<double>(now - start).count();
            if (wall >= duration)
                break;
            auto pump_start = std::chrono::steady_clock::now();
            player.pump();
            double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pump_start).count();
            max_pump = std::max(max_pump, ms);
            sum_pump += ms;
            ++pumps;
            if (auto frame = player.take_frame()) {
                last_pts = frame->timestamp;
                ++frames;
            }
            if (wall >= report) {
                std::cout << std::fixed << std::setprecision(4) << "wall=" << wall << " pos=" << player.position()
                          << " played=" << player.played_audio_frames()
                          << " buffered=" << player.buffered_audio_seconds() << " frame=" << last_pts
                          << " published=" << frames << " decoded=" << player.statistics().frames
                          << " downloads=" << player.statistics().hardware_downloads
                          << " download_ms=" << player.statistics().hardware_download_ms
                          << " prep_ms=" << player.statistics().preparation_ms << " pump_mean_ms=" << sum_pump / pumps
                          << " pump_max_ms=" << max_pump << " decoder=" << player.info().decoder << std::endl;
                report += 1;
            }
            if (player.playback_state() == openxmb::video::state::failed) {
                std::cerr << player.error() << std::endl;
                return 4;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        if (argc > 7 && std::string(argv[7]) == "--require-realtime") {
            const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (std::abs(wall - player.position()) > 0.5 || player.position() - last_pts > 0.25) {
                std::cerr << "Backend clock/presentation did not sustain real time" << std::endl;
                return 5;
            }
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    if (mixer)
        Mix_CloseAudio();
}
