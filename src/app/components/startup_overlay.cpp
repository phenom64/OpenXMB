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
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "openxmb/xmb/boot_layout.hpp"
#include "openxmb/xmb/boot_timeline.hpp"
#include "openxmb/xmb/identity.hpp"

module openxmb.app;

import sdl2;
import spdlog;
import auroreengine;
import glm;
import openxmb.config;

import :startup_overlay;

namespace app {

namespace {
std::vector<std::string> wrap_warning_body(
    auroreengine::gui_renderer& renderer,
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
  if(startup_channel >= 0 && sdl::mix::GetChunk(startup_channel) == chunk &&
     sdl::mix::Playing(startup_channel)) {
    sdl::mix::HaltChannel(startup_channel);
  }
  if(chunk) {
    sdl::mix::FreeChunk(chunk);
  }
}

result startup_overlay::tick(app::shell* xmb) {
  host = xmb;
  // Start audio on first tick to ensure mixer is ready
  if (!started_audio) {
    const std::array sound_paths{
      config::CONFIG.asset_directory/"sounds/startup.ogg",
      config::CONFIG.asset_directory/"sounds/startup.wav",
      config::CONFIG.asset_directory/"sounds/NSE.startup.ogg",
      config::CONFIG.asset_directory/"sounds/NSE.startup.GameBoot.wav"
    };

    for(const auto& path : sound_paths) {
      if(!config::CONFIG.enabled("audio.startup")) break;
      auto path_string = path.string();
      auto* chunk = sdl::mix::LoadWAV(path_string.c_str());
      if(!chunk) {
        continue;
      }

      startup_sound = chunk;
      sdl::mix::VolumeChunk(chunk, static_cast<int>(112*std::clamp(config::CONFIG.number("audio.master",100)/100.0,0.0,1.0)));
      startup_channel = sdl::mix::FadeInChannel(-1, chunk, 0, 160);
      if(startup_channel == -1) {
        startup_channel = sdl::mix::PlayChannel(-1, chunk, 0);
      }
      if(startup_channel == -1) {
        spdlog::debug("startup_overlay: PlayChannel error for {}: {}", path_string, sdl::mix::GetError());
      } else {
        spdlog::debug("Startup cue started");
      }
      break;
    }
    if(!startup_sound) {
      spdlog::debug("startup_overlay: failed to load startup sound from {}", (config::CONFIG.asset_directory/"sounds").string());
    }
    started_audio = true;
  }

  const auto sample = xmb->startup_sample();
  warning_visible = sample.warning_opacity > 0.0;
  if(sample.complete && !visual_complete) spdlog::debug("Startup UI reveal complete");
  visual_complete = sample.complete;
  revealing_menu = sample.ui_reveal > 0.0;
  if(xmb != nullptr) {
    xmb->set_startup_background_blur_px(static_cast<float>(
        sample.complete ? 0.0 : sample.warning_backdrop_blur_px));
  }
  // Visual transitions never own the audio lifetime. Keep a transparent,
  // input-free component alive if a longer startup cue is still playing.
  const bool audio_playing = startup_channel >= 0 &&
      sdl::mix::GetChunk(startup_channel) == startup_sound &&
      sdl::mix::Playing(startup_channel);
  if(startup_channel >= 0 && !audio_playing && !audio_finished_logged) {
    spdlog::debug("Startup cue finished");
    audio_finished_logged = true;
  }
  if (sample.complete && !audio_playing) {
    return result::close;
  }
  return result::success;
}

result startup_overlay::on_event(const event& input) {
  if(visual_complete) return result::unsupported;
  const bool enter = input.test<events::key_down>([](const auto& key) {
    return key.keycode == 40 || key.keycode == 88; // Return / keypad Enter scancodes
  });
  const bool cross = input.test<events::controller_button_down>([](const auto& button) {
    return button.button == events::logical_controller_button::a;
  });
  if(host != nullptr && warning_visible && (enter || cross || input.action == action::ok)) {
    host->dismiss_startup_warning(input);
    warning_visible = false;
  }
  // Consume intro input so the same confirm cannot activate the hidden menu.
  return result::success;
}

void startup_overlay::render(auroreengine::gui_renderer& renderer, app::shell* xmb) {
  const auto sample = xmb->startup_sample();
  if(sample.complete) return;
  // The firmware's boot background is not veiled: during the identity and
  // warning it already has its steady brightness (measured on RPCS3).

  const auto identity=openxmb::xmb::kStartupIdentity;
  float pixels=34.0F*renderer.frame_size.height/1080.0F;
  const float available=static_cast<float>(openxmb::xmb::kBootIdentityMaxWidth)*renderer.frame_size.width;
  const float measured=renderer.text_width_pixels(identity,pixels);
  if(measured>available) pixels*=available/measured;
  const float width=renderer.text_width_pixels(identity,pixels);
  const float right=static_cast<float>(openxmb::xmb::kBootIdentityRightX)*renderer.frame_size.width;
  const float baseline=static_cast<float>(openxmb::xmb::kBootIdentityCenterY)*renderer.frame_size.height+pixels*0.35F;
  renderer.draw_text_pixels(identity,right-width,baseline,pixels,{1.0F,0.96F,0.88F,1.0F},0.0F,0.0F,false,
      {static_cast<float>(sample.identity_gradient[0]),static_cast<float>(sample.identity_gradient[1]),static_cast<float>(sample.identity_gradient[2])});

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
