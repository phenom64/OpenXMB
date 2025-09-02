/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025 Syndromatic Ltd. All rights reserved
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

module;

#include <algorithm>
#include <chrono>

module openxmb.app;

import sdl2;
import spdlog;
import dreamrender;
import glm;
import openxmb.config;

import :startup_overlay;

namespace app {

static float compute_opacity(std::chrono::milliseconds t) {
  using namespace std::chrono;
  const auto fade_in = 600ms;
  const auto hold = 1600ms;
  const auto fade_out = 900ms;
  if (t <= 0ms) return 0.0f;
  if (t < fade_in) {
    return static_cast<float>(t.count()) / static_cast<float>(fade_in.count());
  }
  if (t < fade_in + hold) {
    return 1.0f;
  }
  if (t < fade_in + hold + fade_out) {
    auto out_t = t - (fade_in + hold);
    return 1.0f - static_cast<float>(out_t.count()) / static_cast<float>(fade_out.count());
  }
  return 0.0f;
}

result startup_overlay::tick(app::shell*) {
  // Start audio on first tick to ensure mixer is ready
  if (!started_audio) {
    // Prefer original OGG; fall back to WAV if OGG decoder is unavailable
    auto ogg_path = (config::CONFIG.asset_directory/"sounds/startup.ogg").string();
    auto wav_path = (config::CONFIG.asset_directory/"sounds/startup.wav").string();

    if (auto* chunk = sdl::mix::LoadWAV(ogg_path.c_str())) {
      if (sdl::mix::PlayChannel(-1, chunk, 0) == -1) {
        spdlog::debug("startup_overlay: PlayChannel error for OGG: {}", sdl::mix::GetError());
      }
    } else if (auto* chunk2 = sdl::mix::LoadWAV(wav_path.c_str())) {
      if (sdl::mix::PlayChannel(-1, chunk2, 0) == -1) {
        spdlog::debug("startup_overlay: PlayChannel error for WAV: {}", sdl::mix::GetError());
      }
    } else {
      spdlog::debug("startup_overlay: failed to load startup sound ({} or {}): {}", ogg_path, wav_path, sdl::mix::GetError());
    }
    started_audio = true;
  }

  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time);
  // Lifetime: 600 + 1600 + 900 = 3100ms
  if (t > std::chrono::milliseconds(3100)) {
    return result::close;
  }
  return result::success;
}

void startup_overlay::render(dreamrender::gui_renderer& renderer, app::shell*) {
  using namespace std::chrono;
  auto t = duration_cast<milliseconds>(std::chrono::system_clock::now() - start_time);
  float opacity = compute_opacity(t);

  // Text: right-align near screen edge
  const std::string text = "Syndromatic Engineering Bharat Britannia";
  const float size = 0.06f;
  auto m = renderer.measure_text(text, size);
  // Align to right edge of logical UI space (0..1 on X)
  const float right_margin_x = 0.08f; // 8% of width
  float x = 1.0f - right_margin_x - m.x;
  float y = 0.5f - m.y/2.0f;
  renderer.draw_text(text, x, y, size, glm::vec4(1.0f, 1.0f, 1.0f, std::clamp(opacity, 0.0f, 1.0f)));
}

}
