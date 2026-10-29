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
#include "openxmb/desktop/preview_audio.hpp"
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void fixture(const fs::path &path) {
  std::ofstream output(path, std::ios::binary);
  auto word = [&](std::uint32_t value, unsigned bytes) { for (unsigned i = 0; i < bytes; ++i) output.put(static_cast<char>(value >> (i * 8))); };
  output.write("RIFF", 4); word(36 + 16000, 4); output.write("WAVEfmt ", 8); word(16, 4);
  word(1, 2); word(1, 2); word(16000, 4); word(32000, 4); word(2, 2); word(16, 2); output.write("data", 4); word(16000, 4);
  for (unsigned i = 0; i < 8000; ++i) word(i % 40 < 20 ? 16000 : static_cast<std::uint16_t>(-16000), 2);
}
}
int main(int argc,char** argv) {
  SDL_SetMainReady();
  const auto folder = fs::temp_directory_path() / ("openxmb-preview-audio-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    require(argc==1||argc==2,"usage: preview_audio_tests [REAL_PREVIEW_AUDIO]");
    fs::create_directories(folder); fixture(folder / "SND0.AT3");
    const auto source=argc==2?fs::u8path(argv[1]):folder/"SND0.AT3";
    require(fs::is_regular_file(source),"preview source exists");
    require(SDL_InitSubSystem(SDL_INIT_AUDIO) == 0, "SDL audio initializes");
    require(Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 512) == 0, "Preview mixer initializes");
    {
      openxmb::desktop::PreviewAudio preview;
      preview.set_path(folder / "missing.ogg");
      preview.set_path(source);
      for (int i = 0; i < 400 && !preview.active(); ++i) { SDL_Delay(5); preview.tick(); }
      require(preview.active() && preview.decoded_bytes() > 8000 && Mix_Playing(-1) == 1, "asynchronous preview decodes, resamples and plays an actual mixer channel");
      require(preview.error().empty() && !preview.loading(), "stale cancelled decode does not replace current preview");
      std::cout<<"Decoded preview bytes: "<<preview.decoded_bytes()<<'\n';
      preview.stop();
      require(!preview.active() && preview.decoded_bytes() == 0 && Mix_Playing(-1) == 0, "leaving selection releases preview channel and decoded data");
    }
    Mix_CloseAudio(); SDL_QuitSubSystem(SDL_INIT_AUDIO); fs::remove_all(folder);
    std::cout << "Async game-preview audio checks passed\n";
  } catch (const std::exception &error) { Mix_CloseAudio(); SDL_QuitSubSystem(SDL_INIT_AUDIO); fs::remove_all(folder); std::cerr << error.what() << '\n'; return 1; }
}
