#define SDL_MAIN_HANDLED
#include <openxmb/video.hpp>
#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace video = openxmb::video;
void check(bool value, const char *message) {
    if(!value) throw std::runtime_error(message);
}

video::frame first(video::session& player) {
    player.pause(true);
    for(int i = 0; i < 200; ++i) {
        player.pump();
        check(player.playback_state() != video::state::failed, player.error().c_str());
        if(auto output = player.take_frame()) return std::move(*output);
    }
    throw std::runtime_error("No paused frame was produced");
}

int main(int argc, char **argv) {
    SDL_SetMainReady();
    try {
        check(argc == 5 || argc == 6, "Usage: test fixture rgba|yuv420p|nv12 bt601|bt709|bt2020 limited|full [benchmark]");
        video::session packed(argv[1], {.hardware_acceleration = "off", .audio_output = false});
        video::session native(argv[1], {.hardware_acceleration = "off", .audio_output = false, .native_yuv = true});
        const auto rgba = first(packed);
        const auto frame = first(native);
        auto expected = std::string(argv[2]) == "nv12" ? video::pixel_format::nv12 :
            std::string(argv[2]) == "yuv420p" ? video::pixel_format::yuv420p : video::pixel_format::rgba;
        auto matrix = std::string(argv[3]) == "bt709" ? video::color_matrix::bt709 :
            std::string(argv[3]) == "bt2020" ? video::color_matrix::bt2020 : video::color_matrix::bt601;
        check(frame.format == expected, "Incorrect native/fallback format");
        check(frame.matrix == matrix, "Incorrect color matrix metadata");
        check(frame.full_range == (std::string(argv[4]) == "full"), "Incorrect color range metadata");
        check(frame.width == rgba.width && frame.height == rgba.height, "Frame dimensions changed between contracts");
        if(expected == video::pixel_format::rgba) {
            check(frame.rgba == rgba.rgba, "Unsupported formats must preserve exact software RGBA fallback");
            check(native.statistics().native_frames == 0 && native.statistics().rgba_frames > 0, "Fallback diagnostics wrong");
        } else {
            check(frame.rgba.empty(), "Native output allocated an RGBA frame");
            std::size_t bytes = 0;
            for(int index = 0; index < (expected == video::pixel_format::nv12 ? 2 : 3); ++index) {
                const auto& plane = frame.planes[index];
                check(plane.width == (index ? (frame.width + 1) / 2 : frame.width), "Odd chroma width truncated");
                check(plane.height == (index ? (frame.height + 1) / 2 : frame.height), "Odd chroma height truncated");
                check(plane.bytes.size() == static_cast<std::size_t>(plane.stride) * plane.height, "Packed plane allocation/stride mismatch");
                bytes += plane.bytes.size();
            }
            check(bytes < rgba.rgba.size(), "Native transfer did not reduce upload bytes");
            // Shader-equivalent color conversion at the flat-color fixture's
            // center. Test independent of the Vulkan runtime and GPU driver.
            if(argc == 5) {
                const int x = frame.width / 2, y = frame.height / 2;
                const auto& chroma = frame.planes[1];
                const bool nv12 = expected == video::pixel_format::nv12;
                double Y = frame.planes[0].bytes[y * frame.planes[0].stride + x];
                double U = chroma.bytes[(y / 2) * chroma.stride + (x / 2) * (nv12 ? 2 : 1)];
                double V = nv12 ? chroma.bytes[(y / 2) * chroma.stride + (x / 2) * 2 + 1] :
                    frame.planes[2].bytes[(y / 2) * frame.planes[2].stride + x / 2];
                Y = frame.full_range ? Y / 255 : (Y - 16) / 219;
                U = (U - 128) / (frame.full_range ? 255 : 224);
                V = (V - 128) / (frame.full_range ? 255 : 224);
                const double kr = matrix == video::color_matrix::bt709 ? 0.2126 : matrix == video::color_matrix::bt2020 ? 0.2627 : 0.299;
                const double kb = matrix == video::color_matrix::bt709 ? 0.0722 : matrix == video::color_matrix::bt2020 ? 0.0593 : 0.114;
                const double rgb[] = {Y + (2 - 2 * kr) * V,
                    Y - kb * (2 - 2 * kb) / (1 - kr - kb) * U - kr * (2 - 2 * kr) / (1 - kr - kb) * V,
                    Y + (2 - 2 * kb) * U};
                for(int channel = 0; channel < 3; ++channel)
                    check(std::abs(std::clamp(rgb[channel], 0.0, 1.0) * 255 - rgba.rgba[(y * frame.width + x) * 4 + channel]) <= 4,
                        "Native plane matrix/range conversion disagrees with swscale reference");
            }
        }
        if(argc == 6) {
            for(auto *player : {&packed, &native}) {
                for(int i = 0; i < 48; ++i) {
                    check(player->seek((i % 20) * 0.05), "Benchmark seek failed");
                    first(*player);
                }
            }
        }
        const auto& a = packed.statistics();
        const auto& b = native.statistics();
        std::cout << argv[1] << " " << frame.width << "x" << frame.height
            << " rgba_ms/frame=" << a.preparation_ms / a.frames
            << " native_ms/frame=" << b.preparation_ms / b.frames
            << " rgba_bytes/frame=" << a.upload_bytes / a.frames
            << " native_bytes/frame=" << b.upload_bytes / b.frames
            << " rgba_frames=" << a.frames << " native_frames=" << b.frames << '\n';
    } catch(const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
