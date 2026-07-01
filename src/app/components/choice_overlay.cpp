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
    std::function<void(unsigned int)> confirm_callback,
    std::function<void()> cancel_callback,
    std::function<void(unsigned int)> preview_callback)
    : choices{std::move(choices)}, confirm_callback{std::move(confirm_callback)},
      cancel_callback{std::move(cancel_callback)},
      preview_callback{std::move(preview_callback)}
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
    if(preview_callback) {
        preview_callback(selection_index);
    }
    return true;
}

void choice_overlay::render(dreamrender::gui_renderer& renderer, class shell* xmb) {
    (void)xmb;

    struct gradient_stop {
        float position;
        glm::vec4 colour;
    };

    // xmb-web's firmware-measured Theme/Colour/Background side panel is a
    // right-side translucent lavender wash, not a recoloured copy of the active
    // month.  Keep the XMB visible behind it and interpolate the measured
    // multi-stop profile with small quads.
    const auto open_elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - opened_time);
    const auto open_linear = std::clamp(
        open_elapsed / std::chrono::duration<double>(open_duration), 0.0, 1.0);
    const auto open_alpha =
        static_cast<float>(open_linear * open_linear * (3.0 - 2.0 * open_linear));
    const float x_shift = (1.0F - open_alpha) * (12.0F / 1920.0F);
    constexpr float panel_native_alpha_scale = 0.90F;
    constexpr float panel_left = 1324.0F / 1920.0F;
    constexpr float panel_right = 1697.0F / 1920.0F;
    constexpr std::array<gradient_stop, 8> stops{{
        {0.000F, {178.0F / 255.0F, 170.0F / 255.0F, 194.0F / 255.0F, 0.00F}},
        {0.043F, {178.0F / 255.0F, 170.0F / 255.0F, 194.0F / 255.0F, 0.55F}},
        {0.142F, {151.0F / 255.0F, 141.0F / 255.0F, 174.0F / 255.0F, 0.88F}},
        {0.300F, {160.0F / 255.0F, 150.0F / 255.0F, 182.0F / 255.0F, 0.80F}},
        {0.470F, {174.0F / 255.0F, 165.0F / 255.0F, 191.0F / 255.0F, 0.65F}},
        {0.651F, {195.0F / 255.0F, 189.0F / 255.0F, 210.0F / 255.0F, 0.45F}},
        {0.820F, {220.0F / 255.0F, 217.0F / 255.0F, 229.0F / 255.0F, 0.22F}},
        {1.000F, {245.0F / 255.0F, 244.0F / 255.0F, 247.0F / 255.0F, 0.00F}},
    }}; 
    for(std::size_t i = 0; i + 1 < stops.size(); ++i) {
        const float x0 = panel_left + x_shift + (panel_right - panel_left) * stops[i].position;
        const float x1 = panel_left + x_shift + (panel_right - panel_left) * stops[i + 1].position;
        auto c0 = stops[i].colour;
        auto c1 = stops[i + 1].colour;
        c0.a *= open_alpha * panel_native_alpha_scale;
        c1.a *= open_alpha * panel_native_alpha_scale;
        renderer.draw_quad(std::array{
            dreamrender::simple_renderer::vertex_data{{x0, 0.0f}, c0, {0.0f, 0.0f}},
            dreamrender::simple_renderer::vertex_data{{x0, 1.0f}, c0, {0.0f, 1.0f}},
            dreamrender::simple_renderer::vertex_data{{x1, 0.0f}, c1, {1.0f, 0.0f}},
            dreamrender::simple_renderer::vertex_data{{x1, 1.0f}, c1, {1.0f, 1.0f}},
        }, dreamrender::simple_renderer::params{});
    }

    auto now = std::chrono::system_clock::now();
    double selected = selection_index;
    auto time_since_transition = std::chrono::duration<double>(now - last_selection_time);
    if(time_since_transition < transition_duration) {
        double p = std::clamp(time_since_transition / transition_duration, 0.0, 1.0);
        p = p * p * (3.0 - 2.0 * p);
        selected = last_selection_index + (selected - last_selection_index) * p;
    }

    constexpr double item_height = 40.0 / 1080.0;
    constexpr double list_top_y = 510.0 / 1080.0;
    constexpr float text_x = 1340.0F / 1920.0F;
    constexpr float swatch_x = 1341.0F / 1920.0F;
    constexpr float swatch_extent = 27.0F / 1080.0F;
    constexpr double panel_bottom = 1020.0 / 1080.0;
    if(!top_action.empty()) {
        renderer.draw_text(top_action, text_x + x_shift, top_action_y, 0.044,
            glm::vec4(1.0F, 1.0F, 1.0F, 0.90F * open_alpha), false, true);
        const auto arrow_offset = std::clamp(
            (14.0F + static_cast<float>(top_action.size()) * 10.4F) / 1920.0F,
            0.035F,
            0.072F);
        const float ax = text_x + x_shift + arrow_offset;
        const float aw = 8.0F / 1920.0F;
        const float ah = 6.0F / 1080.0F;
        const auto arrow_colour = glm::vec4(1.0F, 1.0F, 1.0F, 0.78F * open_alpha);
        renderer.draw_quad(std::array{
            dreamrender::simple_renderer::vertex_data{{ax, top_action_y - ah}, arrow_colour, {0.0F, 0.0F}},
            dreamrender::simple_renderer::vertex_data{{ax, top_action_y + ah}, arrow_colour, {0.0F, 1.0F}},
            dreamrender::simple_renderer::vertex_data{{ax + aw, top_action_y}, arrow_colour, {1.0F, 0.5F}},
            dreamrender::simple_renderer::vertex_data{{ax + aw, top_action_y}, arrow_colour, {1.0F, 0.5F}},
        }, dreamrender::simple_renderer::params{});
    }

    const auto visible_capacity = static_cast<std::size_t>(std::max(
        1.0, std::floor((panel_bottom - list_top_y) / item_height)));

    std::size_t first_visible = 0;
    if(choices.size() > visible_capacity) {
        const std::size_t selected_index = static_cast<std::size_t>(selection_index);
        if(selected_index >= visible_capacity - 1) {
            first_visible = selected_index - (visible_capacity - 2);
        }
        if(first_visible + visible_capacity > choices.size()) {
            first_visible = choices.size() - visible_capacity;
        }
    }
    const std::size_t last_visible = choices.empty()
        ? 0
        : std::min(choices.size() - 1, first_visible + visible_capacity - 1);
    const bool colour_chooser = !swatches.empty();

    for(size_t i = first_visible; i <= last_visible && i < choices.size(); i++) {
        const double focus = 1.0 - std::clamp(std::abs(static_cast<double>(i) - selected), 0.0, 1.0);
        const double eased_focus = focus * focus * (3.0 - 2.0 * focus);
        const bool focused = i == selection_index;

        const double size = glm::mix(0.044, 0.052, eased_focus);
        const float alpha = static_cast<float>(glm::mix(0.84, 1.0, eased_focus));

        const float y = static_cast<float>(
            list_top_y + static_cast<double>(i - first_visible) * item_height);
        if(colour_chooser && !focused && i < swatches.size()) {
            glm::vec3 c = swatches[i];
            glm::vec2 swatch_pos{
                swatch_x + x_shift,
                y - swatch_extent * 0.5F,
            };
            glm::vec2 swatch_size{
                swatch_extent / static_cast<float>(renderer.aspect_ratio),
                swatch_extent,
            };
            renderer.draw_rect(
                swatch_pos - glm::vec2{0.0015F, 0.0015F},
                swatch_size + glm::vec2{0.003F, 0.003F},
                glm::vec4(0.0F, 0.0F, 0.0F, 0.34F));
            renderer.draw_rect(swatch_pos, swatch_size, glm::vec4(c, open_alpha));
            continue;
        }
        const std::string& entry = choices[i];
        if(eased_focus > 0.02) {
            float px = 1.2f / static_cast<float>(renderer.frame_size.width);
            float py = 1.2f / static_cast<float>(renderer.frame_size.height);
            glm::vec4 glow(1.0f, 1.0f, 1.0f, static_cast<float>(0.12 * eased_focus));
            glow.a *= open_alpha;
            renderer.draw_text(entry, text_x + x_shift + px, y + py, size, glow, false, true);
            renderer.draw_text(entry, text_x + x_shift - px, y - py, size, glow * 0.65f, false, true);
        }
        renderer.draw_text(entry, text_x + x_shift, y, size,
            glm::vec4(1, 1, 1, alpha * open_alpha), false, true);
    }

    constexpr float arrow_size = 0.035F;
    if(first_visible > 0) {
        renderer.draw_text("▲", text_x + x_shift,
            static_cast<float>(list_top_y - item_height),
            arrow_size, glm::vec4(1.0F, 1.0F, 1.0F, 0.85F * open_alpha), false, true);
    }
    if(!choices.empty() && last_visible + 1 < choices.size()) {
        renderer.draw_text("▼", text_x + x_shift,
            static_cast<float>(list_top_y +
                static_cast<double>(last_visible - first_visible + 1) * item_height),
            arrow_size, glm::vec4(1.0F, 1.0F, 1.0F, 0.85F * open_alpha), false, true);
    }
}

}
