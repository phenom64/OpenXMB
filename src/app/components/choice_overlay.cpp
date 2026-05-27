/* XMBShell, a console-like desktop shell
 * Copyright (C) 2025 - JCM
 *
 * This file (or substantial portions of it) is derived from XMBShell:
 *   https://github.com/JnCrMx/xmbshell
 *
 * Modified by Syndromatic Ltd for OpenXMB.
 * Portions Copyright (C) 2025 Syndromatic Ltd, Kavish Krishnakumar.
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
#include <functional>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

module openxmb.app;
import :choice_overlay;

import dreamrender;
import glm;
import openxmb.utils;
import vulkan_hpp;
import vma;

namespace app {

choice_overlay::choice_overlay(std::vector<std::string> choices, unsigned int selection_index,
    std::function<void(unsigned int)> confirm_callback, std::function<void()> cancel_callback)
    : choices{std::move(choices)}, confirm_callback{std::move(confirm_callback)}, cancel_callback{std::move(cancel_callback)}
{
    if(this->choices.empty()) {
        this->selection_index = 0;
        this->last_selection_index = 0;
    } else {
        this->selection_index = std::min<unsigned int>(selection_index, static_cast<unsigned int>(this->choices.size() - 1));
        this->last_selection_index = this->selection_index;
    }
}

result choice_overlay::on_action(action action) {
    switch(action) {
        case action::cancel:
            if(cancel_callback) {
                cancel_callback();
            }
            return result::close | result::back_sound;
        case action::ok:
            if(choices.empty()) {
                return result::unsupported | result::error_rumble;
            }
            if(confirm_callback) {
                confirm_callback(selection_index);
            }
            return result::close | result::confirm_sound;
        case action::up:
            return select_relative(action::up) ? result::success | result::ok_sound : result::unsupported | result::error_rumble;
        case action::down:
            return select_relative(action::down) ? result::success | result::ok_sound : result::unsupported | result::error_rumble;
        default:
            return result::unsupported | result::error_rumble;
    }
}

bool choice_overlay::select_relative(action dir) {
    if(choices.empty()) {
        return false;
    }

    if(dir == action::up) {
        if(selection_index <= 0) {
            return false;
        }
        last_selection_index = selection_index;
        last_selection_time = std::chrono::system_clock::now();
        selection_index = (selection_index + choices.size() - 1) % choices.size();
    } else if(dir == action::down) {
        if(selection_index >= choices.size() - 1) {
            return false;
        }
        last_selection_index = selection_index;
        last_selection_time = std::chrono::system_clock::now();
        selection_index = (selection_index + 1) % choices.size();
    } else {
        return false;
    }
    return true;
}

void choice_overlay::render(dreamrender::gui_renderer& renderer, class shell* xmb) {
    (void)xmb;

    // Sidebar gradient that adapts to the current theme colour (slightly lighter/darker)
    glm::vec3 base = utils::xmb_resolve_theme_colour(std::chrono::system_clock::now()).shaded_colour;
    glm::vec4 leftCol  = glm::vec4(glm::clamp(base*1.10f, 0.0f, 1.0f), 1.0f);
    glm::vec4 rightCol = glm::vec4(glm::clamp(base*0.35f, 0.0f, 1.0f), 0.0f);
    renderer.draw_quad(std::array{
        dreamrender::simple_renderer::vertex_data{{0.65f, 0.0f}, leftCol,  {0.0f, 0.0f}},
        dreamrender::simple_renderer::vertex_data{{0.65f, 1.0f}, leftCol,  {0.0f, 1.0f}},
        dreamrender::simple_renderer::vertex_data{{0.90f, 0.0f}, rightCol, {1.0f, 0.0f}},
        dreamrender::simple_renderer::vertex_data{{0.90f, 1.0f}, rightCol, {1.0f, 1.0f}},
    }, dreamrender::simple_renderer::params{});

    auto now = std::chrono::system_clock::now();
    double selected = selection_index;
    auto time_since_transition = std::chrono::duration<double>(now - last_selection_time);
    if(time_since_transition < transition_duration) {
        double p = std::clamp(time_since_transition / transition_duration, 0.0, 1.0);
        p = p * p * (3.0 - 2.0 * p);
        selected = last_selection_index + (selected - last_selection_index) * p;
    }

    const bool compact = choices.size() > 8;
    const double base_size = compact ? 0.058 : 0.070;
    const double item_height = compact ? 0.046 : 0.058;
    constexpr glm::vec2 base_pos = {0.675f, 0.425f};

    double offsetY = -selected*item_height;

    for(size_t i=0; i<choices.size(); i++) {
        double focus = 1.0 - std::clamp(std::abs(static_cast<double>(i) - selected), 0.0, 1.0);
        focus = focus * focus * (3.0 - 2.0 * focus);

        double size = base_size * glm::mix(0.74, 1.0, focus);
        float alpha = static_cast<float>(glm::mix(0.58, 1.0, focus));

        // Optional colour swatch square
        float y = base_pos.y + offsetY + item_height*i;
        if(i < swatches.size()) {
            glm::vec3 c = swatches[i];
            float sq = size*0.6f; // square size relative to text size
            glm::vec2 swatch_pos{base_pos.x - 0.032f/static_cast<float>(renderer.aspect_ratio), y - sq/2.0f};
            glm::vec2 swatch_size{sq/static_cast<float>(renderer.aspect_ratio), sq};
            renderer.draw_rect(swatch_pos - glm::vec2{0.0015f, 0.0015f}, swatch_size + glm::vec2{0.003f, 0.003f}, glm::vec4(0.0f, 0.0f, 0.0f, 0.34f));
            renderer.draw_rect(swatch_pos, swatch_size, glm::vec4(c, alpha));
        }
        const std::string& entry = choices[i];
        if(focus > 0.02) {
            float px = 1.2f / static_cast<float>(renderer.frame_size.width);
            float py = 1.2f / static_cast<float>(renderer.frame_size.height);
            glm::vec4 glow(1.0f, 1.0f, 1.0f, static_cast<float>(0.12 * focus));
            renderer.draw_text(entry, base_pos.x + px, y + py, size, glow, false, true);
            renderer.draw_text(entry, base_pos.x - px, y - py, size, glow * 0.65f, false, true);
        }
        renderer.draw_text(entry, base_pos.x, y, size, glm::vec4(1, 1, 1, alpha), false, true);
    }
}

}
