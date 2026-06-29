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
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "openxmb/xmb/boot_layout.hpp"
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

std::optional<double> fixed_boot_seconds_from_environment() {
  const char* value = std::getenv("OPENXMB_FIXED_BOOT_SECONDS");
  if(value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  const std::string_view text{value};
  double seconds{};
  const auto [end, error] = std::from_chars(
      text.data(), text.data() + text.size(), seconds,
      std::chars_format::general);
  if(error != std::errc{} || end != text.data() + text.size() ||
     !std::isfinite(seconds) || seconds < 0.0) {
    spdlog::warn(
        "Ignoring invalid OPENXMB_FIXED_BOOT_SECONDS value '{}'; expected a finite non-negative number",
        text);
    return std::nullopt;
  }
  return seconds;
}

double boot_elapsed_seconds(std::chrono::steady_clock::time_point start) {
  if(const auto fixed = fixed_boot_seconds_from_environment()) {
    return *fixed;
  }
  return elapsed_seconds(start);
}

std::vector<std::string> wrap_warning_body(
    dreamrender::gui_renderer& renderer,
    std::string_view body,
    float font_size,
    float max_width) {
  std::vector<std::string> lines;
  std::string line;
  std::size_t start = 0;
  while(start < body.size()) {
    const auto space = body.find(' ', start);
    const auto end = space == std::string_view::npos ? body.size() : space;
    const auto word = body.substr(start, end - start);
    std::string candidate = line.empty()
        ? std::string(word)
        : line + ' ' + std::string(word);
    const float visual_width = renderer.measure_text(candidate, font_size).x *
                               static_cast<float>(openxmb::xmb::kBootNativeTextMeasureToVisualScale);
    if(!line.empty() && visual_width > max_width) {
      lines.push_back(line);
      line = std::string(word);
    } else {
      line = std::move(candidate);
    }
    start = end + (space == std::string_view::npos ? 0 : 1);
    if(space == std::string_view::npos) {
      break;
    }
  }
  if(!line.empty()) {
    lines.push_back(line);
  }
  return lines;
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

  const auto elapsed = boot_elapsed_seconds(start_time);
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
  const auto elapsed = boot_elapsed_seconds(start_time);
  const auto sample = openxmb::xmb::sample_boot_timeline(elapsed);

  const auto settle_start = openxmb::xmb::BootMilestones::warning_out_seconds;
  const auto settle_end = openxmb::xmb::BootMilestones::ui_in_seconds;
  const auto settle = smooth01(static_cast<float>(
      (elapsed - settle_start) / (settle_end - settle_start)));
  renderer.draw_rect(
      glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 1.0f),
      glm::vec4(0.005f, 0.0f, 0.012f, 0.88f * (1.0f - settle)));

  // Preserve the exact startup identity words, but lay them out as a right-side
  // two-line logo replacement so the long phrase does not shrink to a tiny
  // single line inside xmb-web's coldboot logo zone.
  constexpr std::array<std::string_view, 2> identity_lines{{
      "Syndromatic Limited",
      "Bharat Britannia",
  }};
  float size = static_cast<float>(openxmb::xmb::kBootIdentityBaseSize) *
               static_cast<float>(sample.identity_scale);
  for(const auto line : identity_lines) {
    const auto measured = renderer.measure_text(line, size);
    const double visual_width =
        static_cast<double>(measured.x) *
        openxmb::xmb::kBootNativeTextMeasureToVisualScale;
    if(visual_width > openxmb::xmb::kBootIdentityMaxWidth) {
      size = static_cast<float>(openxmb::xmb::fit_startup_identity_size(
          size, visual_width));
    }
  }
  std::array<openxmb::xmb::BootTextMeasure, identity_lines.size()> line_measures{};
  double line_height = 0.0;
  for(std::size_t index = 0; index < identity_lines.size(); ++index) {
    const auto measured = renderer.measure_text(identity_lines[index], size);
    line_measures[index] = {
        .width = static_cast<double>(measured.x) *
                 openxmb::xmb::kBootNativeTextMeasureToVisualScale,
        .height = static_cast<double>(measured.y) *
                  openxmb::xmb::kBootNativeTextMeasureToVisualScale,
    };
    line_height = std::max(line_height, line_measures[index].height);
  }
  const float identity_pitch = static_cast<float>(
      line_height * openxmb::xmb::kBootIdentityLinePitchScale);
  const float identity_top = static_cast<float>(
      openxmb::xmb::kBootIdentityCenterY -
      ((line_height +
        static_cast<double>(identity_lines.size() - 1) * identity_pitch) *
       0.5));
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
    for(std::size_t index = 0; index < identity_lines.size(); ++index) {
      const auto placement =
          openxmb::xmb::place_startup_identity_text(line_measures[index]);
      const float x = static_cast<float>(placement.x);
      const float y = identity_top + static_cast<float>(index) * identity_pitch;
      for(const auto direction : directions) {
        renderer.draw_text(
            identity_lines[index], x + direction.x * blur_x,
            y + direction.y * blur_y, size,
            glm::vec4(1.0f, 0.88f, 0.68f, identity_alpha * 0.08f));
      }
    }
  }
  for(std::size_t index = 0; index < identity_lines.size(); ++index) {
    const auto placement =
        openxmb::xmb::place_startup_identity_text(line_measures[index]);
    renderer.draw_text(
        identity_lines[index], static_cast<float>(placement.x),
        identity_top + static_cast<float>(index) * identity_pitch, size,
        glm::vec4(1.0f, 0.96f, 0.88f, identity_alpha));
  }

  const float warning_alpha =
      std::clamp(static_cast<float>(sample.warning_opacity), 0.0f, 1.0f);
  if(warning_alpha > 0.0f) {
    const std::string title = "PHOTOSENSITIVE EPILEPSY";
    constexpr std::string_view warning_body =
        "IF YOU HAVE A HISTORY OF EPILEPSY OR SEIZURES, CONSULT A DOCTOR "
        "BEFORE USE. CERTAIN PATTERNS MAY TRIGGER SEIZURES WITH NO PRIOR "
        "HISTORY. BEFORE USING THIS PRODUCT, CAREFULLY READ THE INSTRUCTION "
        "MANUAL.";
    const float body_size =
        static_cast<float>(openxmb::xmb::kBootWarningFontSize);
    const float wrap_width =
        static_cast<float>(openxmb::xmb::kBootWarningWrapWidth);
    std::vector<std::string> warning_lines{title};
    auto wrapped_body =
        wrap_warning_body(renderer, warning_body, body_size, wrap_width);
    warning_lines.insert(warning_lines.end(),
                         std::make_move_iterator(wrapped_body.begin()),
                         std::make_move_iterator(wrapped_body.end()));

    std::vector<double> line_widths;
    line_widths.reserve(warning_lines.size());
    for(const auto& line : warning_lines) {
      line_widths.push_back(renderer.measure_text(line, body_size).x *
                            openxmb::xmb::kBootNativeTextMeasureToVisualScale);
    }
    const auto warning_layout =
        openxmb::xmb::place_boot_warning_block(line_widths);
    float line_y = static_cast<float>(warning_layout.first_top_y);
    const float shadow_y = 2.0F / static_cast<float>(renderer.frame_size.height);
    for(const auto& line : warning_lines) {
      renderer.draw_text(
          line, static_cast<float>(warning_layout.x), line_y + shadow_y,
          body_size, glm::vec4(0.0f, 0.0f, 0.0f, warning_alpha * 0.55f));
      renderer.draw_text(
          line, static_cast<float>(warning_layout.x), line_y, body_size,
          glm::vec4(1.0f, 1.0f, 1.0f, warning_alpha));
      line_y += static_cast<float>(warning_layout.line_pitch);
    }
  }
}

}
