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

module;

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>

module openxmb.app;

import sdl2;
import spdlog;
import dreamrender;
import glm;
import openxmb.config;

import :startup_overlay;

namespace app {

namespace {
float smooth01(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return value * value * (3.0f - 2.0f * value);
}

static float compute_opacity(std::chrono::milliseconds t) {
  using namespace std::chrono;
  const auto fade_in = 720ms;
  const auto hold = 1700ms;
  const auto fade_out = 980ms;
  if (t <= 0ms) return 0.0f;
  if (t < fade_in) {
    return smooth01(static_cast<float>(t.count()) / static_cast<float>(fade_in.count()));
  }
  if (t < fade_in + hold) {
    return 1.0f;
  }
  if (t < fade_in + hold + fade_out) {
    auto out_t = t - (fade_in + hold);
    return 1.0f - smooth01(static_cast<float>(out_t.count()) / static_cast<float>(fade_out.count()));
  }
  return 0.0f;
}
}

startup_overlay::~startup_overlay() {
  auto* chunk = static_cast<sdl::mix::Chunk*>(startup_sound);
  if(startup_channel >= 0 && sdl::mix::Playing(startup_channel)) {
    sdl::mix::HaltChannel(startup_channel);
  }
  if(chunk) {
    sdl::mix::FreeChunk(chunk);
  }
}

result startup_overlay::tick(app::shell*) {
  // Start audio on first tick to ensure mixer is ready
  if (!started_audio) {
    const std::array sound_paths{
      config::CONFIG.asset_directory/"sounds/startup.ogg",
      config::CONFIG.asset_directory/"sounds/startup.wav",
      config::CONFIG.asset_directory/"sounds/NSE.startup.ogg",
      config::CONFIG.asset_directory/"sounds/NSE.startup.GameBoot.wav"
    };

    for(const auto& path : sound_paths) {
      auto path_string = path.string();
      auto* chunk = sdl::mix::LoadWAV(path_string.c_str());
      if(!chunk) {
        continue;
      }

      startup_sound = chunk;
      sdl::mix::VolumeChunk(chunk, 112);
      startup_channel = sdl::mix::FadeInChannel(-1, chunk, 0, 160);
      if(startup_channel == -1) {
        startup_channel = sdl::mix::PlayChannel(-1, chunk, 0);
      }
      if(startup_channel == -1) {
        spdlog::debug("startup_overlay: PlayChannel error for {}: {}", path_string, sdl::mix::GetError());
      }
      break;
    }
    if(!startup_sound) {
      spdlog::debug("startup_overlay: failed to load startup sound from {}", (config::CONFIG.asset_directory/"sounds").string());
    }
    started_audio = true;
  }

  auto now = std::chrono::steady_clock::now();
  auto t = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time);
  if(!fading_audio && t > std::chrono::milliseconds(2920) && startup_channel >= 0 && sdl::mix::Playing(startup_channel)) {
    sdl::mix::FadeOutChannel(startup_channel, 380);
    fading_audio = true;
  }
  if (t > std::chrono::milliseconds(3520)) {
    return result::close;
  }
  return result::success;
}

void startup_overlay::render(dreamrender::gui_renderer& renderer, app::shell*) {
  using namespace std::chrono;
  auto t = duration_cast<milliseconds>(std::chrono::steady_clock::now() - start_time);
  float opacity = compute_opacity(t);
  float alpha = std::clamp(opacity, 0.0f, 1.0f);

  renderer.draw_rect(glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 0.92f * alpha));

  // Text: right-align near screen edge
  const std::string text = "Syndromatic Engineering Bharat Britannia";
  float size = 0.054f;
  auto m = renderer.measure_text(text, size);
  if(m.x > 0.82f) {
    size *= 0.82f / m.x;
    m = renderer.measure_text(text, size);
  }
  // Align to right edge of logical UI space (0..1 on X)
  const float right_margin_x = 0.08f; // 8% of width
  float settle = smooth01(static_cast<float>(t.count()) / 820.0f);
  float x = 1.0f - right_margin_x - m.x + (1.0f - settle) * 0.018f;
  float y = 0.5f - m.y/2.0f;
  float px = 1.4f / static_cast<float>(renderer.frame_size.width);
  float py = 1.4f / static_cast<float>(renderer.frame_size.height);
  float shimmer = 0.5f + 0.5f * std::sin(static_cast<float>(t.count()) * 0.0055f);
  renderer.draw_text(text, x + px, y + py, size, glm::vec4(0.0f, 0.0f, 0.0f, 0.50f * alpha));
  renderer.draw_text(text, x, y, size, glm::vec4(1.0f, 1.0f, 1.0f, alpha));
  renderer.draw_rect(glm::vec2(x, y + m.y + 0.010f),
                     glm::vec2(m.x, std::max(1.0f / renderer.frame_size.height, 0.0012f)),
                     glm::vec4(1.0f, 1.0f, 1.0f, alpha * (0.10f + 0.08f * shimmer)));
}

}
