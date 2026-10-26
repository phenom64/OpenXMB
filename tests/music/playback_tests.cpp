#include "openxmb/music/player.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <stdexcept>

using namespace openxmb::music;
namespace {
void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
void wave(const std::filesystem::path &file, double duration) {
  std::ofstream output(file, std::ios::binary);
  auto word = [&](std::uint32_t value, unsigned bytes) {
    for (unsigned index = 0; index < bytes; ++index) output.put(static_cast<char>(value >> (index * 8)));
  };
  const auto samples = static_cast<std::uint32_t>(duration * 44100);
  output.write("RIFF", 4); word(36 + samples * 2, 4); output.write("WAVEfmt ", 8); word(16, 4);
  word(1, 2); word(1, 2); word(44100, 4); word(88200, 4); word(2, 2); word(16, 2);
  output.write("data", 4); word(samples * 2, 4);
  for (std::uint32_t index = 0; index < samples; ++index)
    word(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::sin(2 * std::numbers::pi * 440 * index / 44100) * 16000)), 2);
}
void run_for(Player &player, unsigned milliseconds) {
  for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 10) {
    SDL_Delay(10);
    player.tick();
  }
}
void format(const std::filesystem::path &path) {
  Player player;
  require(player.set_queue({{path, "Fixture"}}), "format open failed: " + path.string() + " " + player.snapshot().error);
  run_for(player, 120);
  require(player.snapshot().status == Status::playing && player.snapshot().position > 0.05,
          "fixture audio must be consumed by SDL output: " + path.string());
  require(player.snapshot().metadata.title == "Fixture Title" && player.snapshot().metadata.artist == "Fixture Artist", "FFmpeg reads embedded music tags");
  if (path.extension() == ".mp3") require(static_cast<bool>(player.snapshot().metadata.artwork), "MP3 embedded album art extracted");
  player.shutdown();
}
void playback(const std::filesystem::path &root) {
  wave(root / "long.wav", 2.0);
  wave(root / "short.wav", 0.20);
  Player player;
  require(player.set_queue({{root / "long.wav", "Long"}, {root / "short.wav", "Short"}}), "PCM playback opens");
  run_for(player, 180);
  require(player.snapshot().position > 0.1 && player.snapshot().metadata.duration > 1.9, "actual decoded audio advances position and reports duration");
  require(std::ranges::any_of(player.snapshot().spectrum, [](float level) { return level > 0.001f; }), "visualizer sees nonzero decoded PCM");
  player.pause();
  const double paused = player.snapshot().position;
  run_for(player, 100);
  require(player.snapshot().status == Status::paused && std::abs(player.snapshot().position - paused) < 0.04, "pause freezes playback position");
  // SDL_mixer's WAV decoder aligns seeks to whole seconds; the transport must
  // still move the actual decoder and resume from that point.
  require(player.play() && player.seek(1.0), "resume and seek supported for PCM");
  run_for(player, 60);
  require(player.snapshot().position >= 0.99, "seek changes decoder position: " + std::to_string(player.snapshot().position));
  player.set_volume(3.0f);
  require(player.snapshot().volume == 1.0f, "volume clamps to output maximum");
  player.set_external_duck(true);
  run_for(player, 150);
  require(player.snapshot().ducked && player.snapshot().effective_volume < 0.4f, "external audio ducks music output");
  player.set_muted(true);
  require(player.snapshot().effective_volume == 0.0f && Mix_VolumeMusic(-1) == 0, "mute reaches actual mixer volume");
  player.set_muted(false);
  player.set_external_duck(false);
  player.stop();
  require(player.snapshot().status == Status::stopped && Mix_PlayingMusic() == 0, "stop halts output");
  require(player.select(1), "queue selection starts selected track");
  run_for(player, 350);
  require(player.snapshot().status == Status::ended, "natural queue end stops without implicit repeat");
  player.set_repeat(Repeat::one);
  require(player.play(), "repeat playback starts");
  const auto generation = player.snapshot().generation;
  run_for(player, 350);
  require(player.snapshot().generation > generation && player.queue().current_index() == 1, "natural repeat-one reloads same decoded track");
  player.set_repeat(Repeat::all);
  require(player.next() && player.queue().current_index() == 0, "manual next wraps repeat-all");
  require(!player.open(root / "missing.mp3") && player.snapshot().status == Status::error && Mix_PlayingMusic() == 0, "missing track yields actionable stopped error");
  player.shutdown();
}
}
int main(int argc, char **argv) {
  const auto root = std::filesystem::temp_directory_path() / ("openxmb-music-playback-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    require(SDL_InitSubSystem(SDL_INIT_AUDIO) == 0, SDL_GetError());
    Mix_Init(MIX_INIT_MP3 | MIX_INIT_FLAC | MIX_INIT_OGG | MIX_INIT_OPUS);
    require(Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 512) == 0, Mix_GetError());
    if (argc > 1) {
      for (int index = 1; index < argc; ++index) format(argv[index]);
    } else {
      std::filesystem::create_directories(root);
      playback(root);
    }
    Mix_CloseAudio();
    Mix_Quit();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    std::filesystem::remove_all(root);
    std::cout << "Music decoder and transport checks passed\n";
  } catch (const std::exception &error) {
    Mix_CloseAudio();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    std::filesystem::remove_all(root);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
