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
#include <string_view>

#include "openxmb/xmb/boot_timeline.hpp"
#include "openxmb/xmb/identity.hpp"

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

double elapsed_seconds(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
      .count();
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

  const auto elapsed = elapsed_seconds(start_time);
  const auto sample = openxmb::xmb::sample_boot_timeline(elapsed);
  if(!fading_audio && elapsed >= openxmb::xmb::BootMilestones::identity_fade_seconds &&
     startup_channel >= 0 && sdl::mix::Playing(startup_channel)) {
    sdl::mix::FadeOutChannel(startup_channel, 900);
    fading_audio = true;
  }
  if (sample.complete) {
    return result::close;
  }
  return result::success;
}

void startup_overlay::render(dreamrender::gui_renderer& renderer, app::shell*) {
  const auto elapsed = elapsed_seconds(start_time);
  const auto sample = openxmb::xmb::sample_boot_timeline(elapsed);

  const auto settle_start = openxmb::xmb::BootMilestones::warning_out_seconds;
  const auto settle_end = openxmb::xmb::BootMilestones::ui_in_seconds;
  const auto settle = smooth01(static_cast<float>(
      (elapsed - settle_start) / (settle_end - settle_start)));
  renderer.draw_rect(
      glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 1.0f),
      glm::vec4(0.005f, 0.0f, 0.012f, 0.88f * (1.0f - settle)));

  const std::string text(openxmb::xmb::kStartupIdentity);
  float size = 0.044f * static_cast<float>(sample.identity_scale);
  auto m = renderer.measure_text(text, size);
  if(m.x > 0.78f) {
    size *= 0.78f / m.x;
    m = renderer.measure_text(text, size);
  }
  const float x = 0.5f - m.x * 0.5f;
  const float y = 0.46f - m.y * 0.5f;
  const float identity_alpha =
      std::clamp(static_cast<float>(sample.identity_opacity), 0.0f, 1.0f);
  const float blur_x = static_cast<float>(sample.identity_blur_px) /
                       static_cast<float>(renderer.frame_size.width);
  const float blur_y = static_cast<float>(sample.identity_blur_px) /
                       static_cast<float>(renderer.frame_size.height);
  if(identity_alpha > 0.0f && sample.identity_blur_px > 0.1) {
    constexpr std::array<glm::vec2, 8> directions{{
        {-1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, 1.0f},
        {-0.7f, -0.7f}, {0.7f, -0.7f}, {-0.7f, 0.7f}, {0.7f, 0.7f},
    }};
    for(const auto direction : directions) {
      renderer.draw_text(
          text, x + direction.x * blur_x, y + direction.y * blur_y, size,
          glm::vec4(1.0f, 0.88f, 0.68f, identity_alpha * 0.08f));
    }
  }
  renderer.draw_text(
      text, x, y, size,
      glm::vec4(1.0f, 0.96f, 0.88f, identity_alpha));

  const float warning_alpha =
      std::clamp(static_cast<float>(sample.warning_opacity), 0.0f, 1.0f);
  if(warning_alpha > 0.0f) {
    const std::string title = "PHOTOSENSITIVE EPILEPSY";
    constexpr std::array<std::string_view, 4> warning_lines{{
        "IF YOU HAVE A HISTORY OF EPILEPSY OR SEIZURES, CONSULT A DOCTOR BEFORE USE.",
        "CERTAIN PATTERNS MAY TRIGGER SEIZURES WITH NO PRIOR HISTORY.",
        "BEFORE USING THIS PRODUCT, CAREFULLY READ THE INSTRUCTION MANUAL.",
        "",
    }};
    const float title_size = 0.026f;
    const auto title_measure = renderer.measure_text(title, title_size);
    renderer.draw_text(
        title, 0.5f - title_measure.x * 0.5f, 0.405f, title_size,
        glm::vec4(1.0f, 1.0f, 1.0f, warning_alpha));
    float line_y = 0.475f;
    for(const auto line : warning_lines) {
      if(line.empty()) continue;
      const std::string line_text(line);
      constexpr float body_size = 0.017f;
      const auto line_measure = renderer.measure_text(line_text, body_size);
      renderer.draw_text(
          line_text, 0.5f - line_measure.x * 0.5f, line_y, body_size,
          glm::vec4(0.92f, 0.92f, 0.92f, warning_alpha));
      line_y += 0.034f;
    }
  }
}

}
