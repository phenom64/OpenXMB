#define SDL_MAIN_HANDLED
#include "../../src/programs/music_reentry.hpp"
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numbers>
#include <stdexcept>
#include <string>

namespace music = openxmb::music;
namespace entry = openxmb::music::reentry;
namespace {
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void wave(const std::filesystem::path &path, double frequency, double seconds = 3) {
    std::ofstream output(path, std::ios::binary);
    const auto samples = static_cast<std::uint32_t>(44100 * seconds);
    auto word = [&](std::uint32_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i)
            output.put(static_cast<char>(value >> (i * 8)));
    };
    output.write("RIFF", 4);
    word(36 + samples * 2, 4);
    output.write("WAVEfmt ", 8);
    word(16, 4);
    word(1, 2);
    word(1, 2);
    word(44100, 4);
    word(88200, 4);
    word(2, 2);
    word(16, 2);
    output.write("data", 4);
    word(samples * 2, 4);
    for (std::uint32_t i = 0; i < samples; ++i)
        word(static_cast<std::uint16_t>(
                 static_cast<std::int16_t>(std::sin(2 * std::numbers::pi * frequency * i / 44100) * 16000)),
             2);
    check(static_cast<bool>(output), "write PCM fixture");
}
std::atomic<float> peak{};
bool floating = false;
void SDLCALL measure(void *, Uint8 *data, int bytes) {
    float measured = 0;
    if (floating)
        for (int i = 0; i < bytes / 4; ++i)
            measured = std::max(measured, std::abs(reinterpret_cast<float *>(data)[i]));
    else
        for (int i = 0; i < bytes / 2; ++i)
            measured = std::max(measured, std::abs(reinterpret_cast<std::int16_t *>(data)[i] / 32768.0F));
    peak.store(std::max(peak.load(), measured));
}
void run(music::Player &player, unsigned milliseconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        player.tick();
        SDL_Delay(5);
    } while (std::chrono::steady_clock::now() < deadline);
    player.tick();
}
void checks(const std::filesystem::path &root) {
    const auto a = root / "Song.wav", b = root / "Other" / "Song.wav", short_song = root / "Short.wav";
    const std::map<std::string, std::string> defaults{{"music.shuffle", "Off"}, {"music.repeat", "Off"}};
    music::Player player;
    check(player.set_queue({{b, "First"}, {a, "Custom active title"}, {b, "Queued duplicate"}}, 1),
          "start custom queue");
    player.set_shuffle(true);
    player.set_repeat(music::Repeat::one);
    player.set_volume(0.37F);
    check(player.seek(0.85), "seek background song");
    Mix_SetPostMix(measure, nullptr);
    peak.store(0);
    run(player, 130);
    check(player.snapshot().position > 0.9 && peak.load() > 0.05F, "real PCM advances before reentry");
    check(entry::same_track(root / "Unavailable.wav", root / "." / "Unavailable.wav"),
          "unavailable current file retains lexical identity");
    const auto low_band = std::max_element(player.snapshot().spectrum.begin(), player.snapshot().spectrum.end()) -
                          player.snapshot().spectrum.begin();
    check(low_band >= 4 && low_band <= 6, "background song contains actual 220 Hz PCM");
    const auto generation = player.snapshot().generation;
    const auto position = player.snapshot().position;
    unsigned replacements = 0;
    auto replace = [&] {
        ++replacements;
        return player.set_queue({{a, "Visible folder title"}, {b, "Folder neighbour"}});
    };
    // Folder/Songs and Albums/Artists supply their own queue lambdas; viewer
    // construction supplies Player::open. All must use this exact guard.
    for (int path = 0; path < 3; ++path) {
        check(entry::open(player, root / "." / "Song.wav", defaults, replace), "reenter current song");
        check(player.snapshot().generation == generation && player.snapshot().position == position,
              "reentry must not reload decoder or reset position");
        check(player.queue().entries().size() == 3 && player.queue().current_index() == 1 &&
                  player.queue().entries()[1].title == "Custom active title",
              "reentry must preserve custom queue and selection");
        check(player.queue().shuffle() && player.queue().repeat() == music::Repeat::one &&
                  player.snapshot().volume == 0.37F,
              "reentry must preserve live transport controls");
    }
    check(replacements == 0, "reentry must not call queue replacement");
    peak.store(0);
    run(player, 100);
    check(player.snapshot().generation == generation && player.snapshot().position > position && peak.load() > 0.05F,
          "real PCM continues after reentry");
    player.pause();
    const auto paused_position = player.snapshot().position;
    check(entry::open(player, a, defaults, replace), "reenter paused song");
    run(player, 45);
    check(player.snapshot().status == music::Status::paused && player.snapshot().position == paused_position &&
              player.snapshot().generation == generation,
          "paused song must reopen paused without advancing");
    // Same inode via another path must also reuse the live decoder.
    const auto alias = root / "Alias.wav";
    std::error_code error;
    std::filesystem::create_hard_link(a, alias, error);
    if (!error)
        check(entry::open(player, alias, defaults, replace) && replacements == 0,
              "hard-link alias identifies same live song");
    // The live decoder still owns valid PCM even if its original file is gone.
    std::filesystem::rename(a, root / "Moved.wav", error);
    if (!error)
        check(entry::open(player, a, defaults, replace) && replacements == 0,
              "same lexical path must preserve live playback after file move");
    check(entry::open(player, b, defaults,
                      [&] {
                          ++replacements;
                          return player.set_queue({{b, "Different selection"}});
                      }),
          "open different track");
    check(replacements == 1 && player.snapshot().generation > generation && player.snapshot().position < 0.05 &&
              player.snapshot().status == music::Status::playing,
          "different track starts a new playback generation");
    check(player.queue().entries().size() == 1 && !player.queue().shuffle() &&
              player.queue().repeat() == music::Repeat::off,
          "new track uses requested queue and configured defaults");
    run(player, 100);
    const auto high_band = std::max_element(player.snapshot().spectrum.begin(), player.snapshot().spectrum.end()) -
                           player.snapshot().spectrum.begin();
    check(high_band >= 10 && high_band <= 12, "different file with same basename actually plays its 880 Hz PCM");
    run(player, 60);
    const auto explicit_generation = player.snapshot().generation;
    const std::map<std::string, std::string> new_defaults{{"music.shuffle", "On"}, {"music.repeat", "All"}};
    check(entry::start_new(player, new_defaults, [&] { return player.set_queue({{b, "Explicit Play Now"}}); }),
          "explicit Play Now replaces current selection");
    check(player.snapshot().generation > explicit_generation && player.snapshot().position < 0.05 &&
              player.queue().shuffle() && player.queue().repeat() == music::Repeat::all,
          "explicit Play Now still restarts and applies new-session defaults");
    player.stop();
    const auto stopped_generation = player.snapshot().generation;
    check(entry::open(player, b, defaults, [&] { return player.open(b); }), "stopped current song restarts");
    check(player.snapshot().status == music::Status::playing && player.snapshot().generation > stopped_generation,
          "stopped current song must not remain stopped");
    check(player.set_queue({{short_song, "Short"}}), "start EOF fixture");
    run(player, 200);
    check(player.snapshot().status == music::Status::ended, "actual PCM reaches natural end");
    const auto ended_generation = player.snapshot().generation;
    check(entry::open(player, short_song, defaults, [&] { return player.open(short_song); }),
          "ended current song restarts");
    check(player.snapshot().status == music::Status::playing && player.snapshot().position < 0.05 &&
              player.snapshot().generation > ended_generation,
          "ended song must start a fresh generation");
    const auto missing = root / "Missing.wav";
    std::filesystem::remove(missing);
    check(!player.open(missing), "missing song establishes error state");
    const auto error_generation = player.snapshot().generation;
    wave(missing, 440);
    check(entry::open(player, missing, defaults, [&] { return player.open(missing); }) &&
              player.snapshot().generation > error_generation,
          "failed same-file playback must retry once the file is available");
    player.shutdown();
    Mix_SetPostMix(nullptr, nullptr);
}
} // namespace
int main() {
    SDL_SetMainReady();
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    const auto root =
        std::filesystem::temp_directory_path() /
        ("openxmb-reentry-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(root);
        wave(root / "Song.wav", 220);
        std::filesystem::create_directories(root / "Other");
        wave(root / "Other" / "Song.wav", 880);
        wave(root / "Short.wav", 440, 0.08);
        check(SDL_InitSubSystem(SDL_INIT_AUDIO) == 0, SDL_GetError());
        for (int output = 0; output < 2; ++output) {
            floating = output != 0;
            check(Mix_OpenAudio(floating ? 48000 : 44100, floating ? AUDIO_F32SYS : AUDIO_S16SYS, 2, 512) == 0,
                  Mix_GetError());
            // Restore a possibly moved file for the second output format.
            if (!std::filesystem::exists(root / "Song.wav"))
                wave(root / "Song.wav", 220);
            checks(root);
            Mix_CloseAudio();
        }
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        std::filesystem::remove_all(root);
        std::cout << "Same-song reentry preserves real PCM, generation, queue and pause; different/stopped/ended/error "
                     "tracks open normally\n";
    } catch (const std::exception &error) {
        Mix_SetPostMix(nullptr, nullptr);
        Mix_CloseAudio();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
