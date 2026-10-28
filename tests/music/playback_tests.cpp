/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025-2026 Syndromatic Ltd. All rights reserved
 * Designed by Kavish Krishnakumar in Manchester.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#define SDL_MAIN_HANDLED
#include "openxmb/music/player.hpp"

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
#include <numbers>
#include <stdexcept>

using namespace openxmb::music;
namespace {
void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
void wave(const std::filesystem::path &file, double duration, bool stepped = false) {
  std::ofstream output(file, std::ios::binary);
  auto word = [&](std::uint32_t value, unsigned bytes) {
    for (unsigned index = 0; index < bytes; ++index) output.put(static_cast<char>(value >> (index * 8)));
  };
  const auto samples = static_cast<std::uint32_t>(duration * 44100);
  output.write("RIFF", 4); word(36 + samples * 2, 4); output.write("WAVEfmt ", 8); word(16, 4);
  word(1, 2); word(1, 2); word(44100, 4); word(88200, 4); word(2, 2); word(16, 2);
  output.write("data", 4); word(samples * 2, 4);
  for (std::uint32_t index = 0; index < samples; ++index)
    word(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::sin(2 * std::numbers::pi * (stepped ? (index < 44100 ? 220 : 880) : 440) * index / 44100) * 16000)), 2);
}
void run_for(Player &player, unsigned milliseconds) {
  for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 10) {
    SDL_Delay(10);
    player.tick();
  }
}
std::atomic<float> output_peak{};
bool float_output{};
void SDLCALL measure(void *, Uint8 *bytes, int length) {
  float peak = 0;
  if (float_output) {
    auto *pcm = reinterpret_cast<float *>(bytes);
    for (int i = 0; i < length / 4; ++i) peak = std::max(peak, std::abs(pcm[i]));
  } else {
    auto *pcm = reinterpret_cast<std::int16_t *>(bytes);
    for (int i = 0; i < length / 2; ++i) peak = std::max(peak, std::abs(pcm[i] / 32768.0f));
  }
  output_peak.store(std::max(output_peak.load(), peak));
}
void signal_and_mixer(const std::filesystem::path &root) {
  const auto fixture = root / "seek-signal.wav";
  // Leave room for scheduler delays while the switch test exercises playback.
  wave(fixture, 8.0, true);
  Player player;
  require(player.set_queue({{fixture, "Seek signal"}}), "signal fixture opens");
  player.set_ducking_enabled(false);
  run_for(player, 130);
  auto peak_band = [&] { return std::max_element(player.snapshot().spectrum.begin(), player.snapshot().spectrum.end()) - player.snapshot().spectrum.begin(); };
  const auto low_band = peak_band();
  require(low_band >= 4 && low_band <= 6, "220 Hz PCM reaches its frequency band");
  require(player.seek(1.213), "signal seeks forward");
  run_for(player, 100);
  const auto high_band = peak_band();
  require(high_band >= 10 && high_band <= 12, "seek decodes 880 Hz audio rather than stale queued 220 Hz samples");
  require(player.seek(0.187), "signal seeks backward");
  run_for(player, 100);
  require(peak_band() == low_band, "backward seek restores actual 220 Hz PCM");
  Mix_SetPostMix(measure, nullptr);
  output_peak.store(0);
  run_for(player, 70);
  require(output_peak.load() > 0.1f, "post-mixer output includes music PCM");
  player.set_muted(true);
  SDL_Delay(30); output_peak.store(0);
  run_for(player, 70);
  require(output_peak.load() == 0, "music gain silences actual mixed PCM");
  // A shell effect remains audible while music is muted. This also proves
  // that music does not replace the mixer's sound-effects callback/output.
  wave(root / "effect.wav", 0.3);
  Mix_Chunk *effect = Mix_LoadWAV((root / "effect.wav").string().c_str());
  require(effect != nullptr, "sound effect loads");
  require(Mix_PlayChannel(-1, effect, 0) >= 0, "sound effect starts alongside music");
  output_peak.store(0); run_for(player, 80);
  require(output_peak.load() > 0.1f, "muted music preserves audible SDL mixer effects");
  player.set_muted(false);
  player.set_ducking_enabled(true);
  const float before_effect = player.snapshot().effective_volume;
  run_for(player, 80);
  require(!player.snapshot().ducked && std::abs(player.snapshot().effective_volume - before_effect) < 0.001f,
          "menu sound effects never duck the music or change its effective volume");
  Mix_HaltChannel(-1); Mix_FreeChunk(effect);
  Mix_SetPostMix(nullptr, nullptr);
  player.set_muted(false);
  player.set_volume(0.6f);
  player.set_output_gain(0.5f);
  require(std::abs(player.snapshot().volume - 0.6f) < 0.001f && std::abs(player.snapshot().effective_volume - 0.3f) < 0.001f, "settings gain preserves per-player user volume");
  player.set_output_gain(1.0f);
  player.pause();
  const auto before_switch = player.snapshot();
  const double device_position = before_switch.position;
  int before_rate = 0, before_channels = 0;
  Uint16 before_format = 0;
  Mix_QuerySpec(&before_rate, &before_format, &before_channels);
  const bool switched = player.set_output_device("System Default");
  const auto after_switch = player.snapshot();
  const auto switch_details = [&] {
    const char *driver = SDL_GetCurrentAudioDriver();
    const auto status_name = [](Status status) {
      switch (status) {
        case Status::stopped: return "stopped";
        case Status::playing: return "playing";
        case Status::paused: return "paused";
        case Status::ended: return "ended";
        case Status::error: return "error";
      }
      return "unknown";
    };
    int after_rate = 0, after_channels = 0;
    Uint16 after_format = 0;
    Mix_QuerySpec(&after_rate, &after_format, &after_channels);
    std::string details = " driver=" + std::string(driver ? driver : "<none>") +
      " before_status=" + status_name(before_switch.status) +
      " before_position=" + std::to_string(before_switch.position) +
      " duration=" + std::to_string(before_switch.metadata.duration) +
      " before_error=" + before_switch.error +
      " before_mixer=" + std::to_string(before_rate) + "/" + std::to_string(before_format) + "/" + std::to_string(before_channels) +
      " after_status=" + status_name(after_switch.status) +
      " after_position=" + std::to_string(after_switch.position) +
      " after_error=" + after_switch.error +
      " after_mixer=" + std::to_string(after_rate) + "/" + std::to_string(after_format) + "/" + std::to_string(after_channels);
    const int count = SDL_GetNumAudioDevices(0);
    details += " device_count=" + std::to_string(count) + " devices=[";
    for (int i = 0; i < count; ++i) {
      const char *name = SDL_GetAudioDeviceName(i, 0);
      details += (i ? ", " : "") + std::string(name ? name : "<null>");
    }
    return details + "] SDL_error=" + SDL_GetError();
  };
  if (!switched) require(false, "default output device reopens:" + switch_details());
  if (!(after_switch.status == Status::paused && std::abs(after_switch.position - device_position) < 0.002))
    require(false, "output switch preserves pause and position:" + switch_details());
  require(!player.set_output_device("OpenXMB nonexistent audio output"), "missing output is rejected");
  require(player.snapshot().status == Status::paused && !player.snapshot().error.empty(), "failed output switch restores transport with actionable error");
  require(player.play(), "music resumes on restored audio output");
  run_for(player, 80);
  require(player.snapshot().position > device_position + 0.04, "restored mixer callback consumes PCM");
  player.shutdown();
}
void format(const std::filesystem::path &path) {
  Player player;
  require(player.set_queue({{path, "Fixture"}}), "format open failed: " + path.string() + " " + player.snapshot().error);
  run_for(player, 120);
  require(player.snapshot().status == Status::playing && player.snapshot().position > 0.05,
          "fixture audio must be consumed by SDL output: " + path.string());
  require(player.snapshot().metadata.title == "Fixture Title" && (path.extension() == ".mod" || player.snapshot().metadata.artist == "Fixture Artist"), "decoder reads embedded music tags: " + path.filename().string());
  require(std::ranges::any_of(player.snapshot().spectrum, [](float level) { return level > 0.001f; }), "decoded format produces real PCM spectrum");
  player.pause();
  const double paused = player.snapshot().position;
  run_for(player, 60);
  require(std::abs(player.snapshot().position - paused) < 0.001, "format pause freezes sample clock");
  require(player.seek(0.713), "subsecond format seek succeeds");
  require(player.snapshot().status == Status::paused && std::abs(player.snapshot().position - 0.713) < 0.002, "seek preserves pause and exact sample position");
  require(player.play(), "format resumes after seek");
  const auto resume_started = std::chrono::steady_clock::now();
  run_for(player, 100);
  const auto resume_elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - resume_started).count();
  // SDL_Delay is a minimum wait: 10 x 10ms took 155ms in a loaded parallel
  // run. Compare sample consumption against measured wall time, allowing only
  // three 512-frame device periods, rather than assuming a 100ms scheduler.
  require(player.snapshot().position > 0.77 && player.snapshot().position <= 0.713 + resume_elapsed + 0.035, "format consumes PCM from seek target: " + path.filename().string() + " position=" + std::to_string(player.snapshot().position) + " wall_seconds=" + std::to_string(resume_elapsed));
  require(player.seek(0.187), "backward format seek succeeds");
  const auto backward_started = std::chrono::steady_clock::now();
  run_for(player, 80);
  const auto backward_elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - backward_started).count();
  require(player.snapshot().position > 0.23 && player.snapshot().position <= 0.187 + backward_elapsed + 0.035, "backward seek removes stale buffered PCM: " + path.filename().string() + " position=" + std::to_string(player.snapshot().position) + " wall_seconds=" + std::to_string(backward_elapsed));
  if (path.extension() == ".mp3") require(static_cast<bool>(player.snapshot().metadata.artwork), "MP3 embedded album art extracted");
  player.shutdown();
}
void playback(const std::filesystem::path &root) {
  wave(root / "long.wav", 2.0);
  wave(root / "short.wav", 0.20);
  Player player;
  require(player.enqueue({{root / "long.wav","Long"},{root / "short.wav","Short"}},true) && player.queue().current()->title=="Long", "Play Next into an empty queue preserves requested track order");
  require(player.set_queue({{root / "long.wav", "Long"}, {root / "short.wav", "Short"}}), "PCM playback opens");
  run_for(player, 180);
  require(player.snapshot().position > 0.1 && player.snapshot().metadata.duration > 1.9, "actual decoded audio advances position and reports duration");
  require(std::ranges::any_of(player.snapshot().spectrum, [](float level) { return level > 0.001f; }), "visualizer sees nonzero decoded PCM");
  player.pause();
  const double paused = player.snapshot().position;
  run_for(player, 100);
  require(player.snapshot().status == Status::paused && std::abs(player.snapshot().position - paused) < 0.04, "pause freezes playback position");
  require(player.play() && player.seek(1.013), "resume and sample-accurate seek supported for PCM");
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
  SDL_SetMainReady();
  const auto root = std::filesystem::temp_directory_path() / ("openxmb-music-playback-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    require(SDL_InitSubSystem(SDL_INIT_AUDIO) == 0, SDL_GetError());
    Mix_Init(MIX_INIT_MP3 | MIX_INIT_FLAC | MIX_INIT_OGG | MIX_INIT_OPUS);
    for (int output = 0; output < 2; ++output) {
      float_output = output == 1;
      require(Mix_OpenAudio(output ? 48000 : 44100, output ? AUDIO_F32SYS : AUDIO_S16SYS, 2, 512) == 0, Mix_GetError());
      if (argc > 1) {
        for (int index = 1; index < argc; ++index) format(argv[index]);
      } else {
        std::filesystem::create_directories(root);
        playback(root);
        signal_and_mixer(root);
      }
      Mix_CloseAudio();
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
