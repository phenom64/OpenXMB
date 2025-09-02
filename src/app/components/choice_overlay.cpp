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

#include <array>
#include <chrono>
#include <functional>
#include <cmath>
#include <string>
#include <vector>

module openxmb.app;
import :choice_overlay;

import dreamrender;
import glm;
import openxmb.config;
import openxmb.utils;
import vulkan_hpp;
import vma;

namespace app {

choice_overlay::choice_overlay(std::vector<std::string> choices, unsigned int selection_index,
    std::function<void(unsigned int)> confirm_callback, std::function<void()> cancel_callback)
    : choices{std::move(choices)}, selection_index(selection_index), last_selection_index{selection_index},
      confirm_callback{confirm_callback}, cancel_callback{cancel_callback}
{

}

result choice_overlay::on_action(action action) {
    switch(action) {
        case action::cancel:
            if(cancel_callback) {
                cancel_callback();
            }
            return result::close;
        case action::ok:
            if(confirm_callback) {
                confirm_callback(selection_index);
            }
            return result::close;
        case action::up:
            return select_relative(action::up) ? result::success | result::ok_sound : result::unsupported | result::error_rumble;
        case action::down:
            return select_relative(action::down) ? result::success | result::ok_sound : result::unsupported | result::error_rumble;
        default:
            return result::unsupported | result::error_rumble;
    }
}

bool choice_overlay::select_relative(action dir) {
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
    // Sidebar gradient that adapts to the current theme colour (slightly lighter/darker)
    glm::vec3 base = config::CONFIG.themeOriginalColour ? utils::xmb_dynamic_colour(std::chrono::system_clock::now())
                                                        : config::CONFIG.themeCustomColour;
    float minuteFrac = (std::chrono::duration<float>(std::chrono::system_clock::now().time_since_epoch()).count()/60.0f);
    int hour = (int)std::fmod(std::chrono::duration<float>(std::chrono::system_clock::now().time_since_epoch()).count()/3600.0f, 24.0f);
    float bright = utils::xmb_hour_brightness(hour, std::fmod(minuteFrac,1.0f));
    base *= bright;
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
        selected = last_selection_index + (selected - last_selection_index) *
            time_since_transition / transition_duration;
    }

    constexpr double base_size = 0.075;
    constexpr double item_height = 0.05;
    constexpr glm::vec2 base_pos = {0.675f, 0.425f};

    double offsetY = -selected*item_height;

    for(int i=0; i<choices.size(); i++) {
        double partial_selection = 0.0;
        if(i == selected) {
            partial_selection = std::clamp(time_since_transition / transition_duration, 0.0, 1.0);
        }

        double size = base_size*glm::mix(0.75, 1.0, partial_selection);

        // Optional colour swatch square
        float y = base_pos.y + offsetY + item_height*i;
        if(i < swatches.size()) {
            glm::vec3 c = swatches[i];
            float sq = size*0.6f; // square size relative to text size
            renderer.draw_rect(glm::vec2{base_pos.x - 0.03f/renderer.aspect_ratio, y - sq/2.0f}, glm::vec2{sq/renderer.aspect_ratio, sq}, glm::vec4(c, 1.0f));
        }
        const std::string& entry = choices[i];
        renderer.draw_text(entry, base_pos.x, y, size, glm::vec4(1, 1, 1, 1), false, true);
    }
}

}
