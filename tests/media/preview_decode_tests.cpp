#include "../../src/menu/media_preview_decode.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

using openxmb::menu_preview::Kind;
namespace fs = std::filesystem;
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: preview_decode_tests FIXTURE_DIRECTORY");
        const fs::path root = fs::path(std::u8string(reinterpret_cast<const char8_t*>(argv[1])));
        int checks = 0;
        auto image = [&](const fs::path& name, Kind kind, int w, int h, int edge = 256, bool blue = false) {
            const auto start = std::chrono::steady_clock::now();
            auto bitmap = openxmb::menu_preview::decode(root/name, kind, edge);
            require(bitmap.width == w && bitmap.height == h, std::string(reinterpret_cast<const char*>(name.u8string().c_str()))+": aspect and bounded dimensions");
            require(bitmap.rgba.size() == static_cast<std::size_t>(w)*h*4, "packed RGBA byte size");
            auto pixel = (static_cast<std::size_t>(h/2)*w+w/2)*4;
            require(bitmap.rgba[pixel+(blue?2:0)] > 180 && bitmap.rgba[pixel+(blue?0:2)] < 65, std::string(reinterpret_cast<const char*>(name.u8string().c_str()))+": decoded colour");
            require(bitmap.rgba[pixel+3] == 255, "opaque alpha");
            require(std::chrono::steady_clock::now()-start < std::chrono::seconds(4), "bounded decode time");
            ++checks;
        };
        auto empty = [&](const fs::path& name, Kind kind) {
            const auto start = std::chrono::steady_clock::now();
            auto bitmap = openxmb::menu_preview::decode(root/name, kind);
            require(bitmap.width == 0 && bitmap.height == 0 && bitmap.rgba.empty(), std::string(reinterpret_cast<const char*>(name.u8string().c_str()))+": expected no preview");
            require(std::chrono::steady_clock::now()-start < std::chrono::seconds(4), "bounded rejection time");
            ++checks;
        };
        image("red.png", Kind::photo, 256, 128);
        image("red.png", Kind::photo, 64, 32, 64);
        image("red.png", Kind::photo, 256, 128, 4096);
        image("jpeg.mp3", Kind::music, 256, 128);
        image("png.m4a", Kind::music, 256, 128);
        image("png.mp3", Kind::music, 256, 128);
        image("jpeg.m4a", Kind::music, 256, 128);
        image("large.m4a", Kind::music, 256, 256);
        image("damaged-embedded/track.mp3", Kind::music, 256, 128);
        image("png.flac", Kind::music, 256, 128);
        image("png.ogg", Kind::music, 256, 128);
        image(fs::path(u8"音楽 café/曲.mp3"), Kind::music, 256, 128);
        image("external/track.flac", Kind::music, 256, 128);
        image("folder/track.mp3", Kind::music, 256, 128);
        image("broken-cover/track.mp3", Kind::music, 256, 128);
        image("movie.mp4", Kind::video, 256, 128, 256, true);
        image("short.mp4", Kind::video, 256, 128);
        image("jpeg.mp3", Kind::video, 256, 128);
        image("red.png", Kind::video, 256, 128);
        image("small.png", Kind::video, 40, 20);
        image("portrait.mp4", Kind::video, 128, 256);
        image("anamorphic.mp4", Kind::video, 256, 128);
        image("movie.mp4", Kind::video, 80, 40, 80, true);
        image("movie.mp4", Kind::video, 1, 1, 0, true);
        empty("noart.mp3", Kind::music);
        empty("noart.m4a", Kind::music);
        empty("invalid.png", Kind::video);
        empty("oversized.png", Kind::video);
        empty("invalid.png", Kind::music);
        empty("missing.mp4", Kind::video);
        empty(".", Kind::video);
        empty("audio.wav", Kind::video);
        empty("truncated.mp4", Kind::video);
        std::cout << checks << " preview decode cases passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}



