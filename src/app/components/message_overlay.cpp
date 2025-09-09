/* XMBShell, a console-like desktop shell
 * Copyright (C) 2025 - JCM
 *
 * This file (or substantial portions of it) is derived from XMBShell:
 *   https://github.com/JnCrMx/xmbshell
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

#include <functional>
#include <ranges>
#include <string>
#include <chrono>
#include <cmath>

module openxmb.app;
import :message_overlay;

import dreamrender;
import glm;
import spdlog;

namespace app {
    message_overlay::message_overlay(std::string title, std::string message, std::vector<std::string> choices,
        std::function<void(unsigned int)> confirm_callback, bool cancelable, std::function<void()> cancel_callback
    ) : title(std::move(title)), message(std::move(message)), choices(std::move(choices)),
        confirm_callback(std::move(confirm_callback)), cancelable(cancelable), cancel_callback(std::move(cancel_callback))
    {
    }

    void message_overlay::render(dreamrender::gui_renderer& renderer, shell* xmb) {
        // Thin top/bottom rules
        renderer.draw_rect(glm::vec2(0.0f, 0.15f), glm::vec2(1.0f, 2.0f/renderer.frame_size.height), glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
        renderer.draw_rect(glm::vec2(0.0f, 0.85f), glm::vec2(1.0f, 2.0f/renderer.frame_size.height), glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));

        // Centered title and message
        const float title_size = 0.05f;
        glm::vec2 tsize = renderer.measure_text(title, title_size);
        renderer.draw_text(title, 0.5f - tsize.x/2.0f, 0.35f - tsize.y, title_size);

        float total_height = 0.0f;
        float max_width = 0.0f;
        const float msg_size = 0.05f;
        for(const auto line : std::views::split(message, '\n')) {
            glm::vec2 s = renderer.measure_text(std::string_view(line), msg_size);
            total_height += s.y;
            max_width = std::max(max_width, s.x);
        }
        float y = 0.50f - total_height * 0.75f; // place slightly under title
        for(const auto line : std::views::split(message, '\n')) {
            std::string_view sv(line);
            glm::vec2 s = renderer.measure_text(sv, msg_size);
            renderer.draw_text(sv, 0.5f - s.x/2.0f, y, msg_size, glm::vec4(1.0));
            y += s.y;
        }

        // Choices (centered) with a soft, pill‑shaped pulsing glow under the selected text
        constexpr float gap = 0.025f;
        float total_width = 0;
        for(const auto& choice : choices) {
            glm::vec2 size = renderer.measure_text(choice, 0.05f);
            total_width += size.x + gap;
        }
        total_width = std::max(0.0f, total_width - gap);
        float x = 0.5f - total_width/2.0f;
        // Pulse for glow
        using clock = std::chrono::steady_clock;
        float pulse = 0.0f;
        {
            auto now = clock::now();
            float t = std::chrono::duration<float>(now - start_time).count();
            pulse = 0.5f + 0.5f*std::sin(t*3.6f); // ~0.57Hz
        }
        for(unsigned int i=0; i<choices.size(); i++) {
            const auto& choice = choices[i];
            glm::vec2 size = renderer.measure_text(choice, 0.05f);
            if(i == selected) {
                // Glyph‑shaped glow: render offset copies in a small ring around the text baseline
                float px = 1.5f / static_cast<float>(renderer.frame_size.width);  // ~1.5 pixels
                float py = 1.5f / static_cast<float>(renderer.frame_size.height);
                // Slightly brighter core; smooth fade via multiple rings
                glm::vec4 glow1 = glm::vec4(1.0f, 1.0f, 1.0f, 0.11f * (0.6f + 0.4f*pulse));
                glm::vec4 glow2 = glm::vec4(1.0f, 1.0f, 1.0f, 0.07f * (0.6f + 0.4f*pulse));
                glm::vec4 glow3 = glm::vec4(1.0f, 1.0f, 1.0f, 0.04f * (0.6f + 0.4f*pulse));
                // Center of the text at base size
                float cx = x + size.x * 0.5f;
                float baseY = 0.62f;
                // Eight-directional offsets (diamond + corners)
                const glm::vec2 offs[12] = {
                    { px,  0}, {-px,  0}, {0,  py}, { 0, -py},
                    { px, py}, {-px, py}, {px, -py}, {-px, -py},
                    {2*px, 0}, {-2*px, 0}, {0, 2*py}, {0, -2*py}
                };
                for(const auto& o : offs) {
                    renderer.draw_text(choice, cx - size.x*0.5f + o.x, baseY + o.y, 0.05f, glow1, false, true);
                }
                // Larger radius, lower alpha
                const glm::vec2 offs2[8] = {
                    { 2*px,  0}, {-2*px,  0}, {0,  2*py}, { 0, -2*py},
                    { 2*px, 2*py}, {-2*px, 2*py}, {2*px, -2*py}, {-2*px, -2*py}
                };
                for(const auto& o : offs2) {
                    renderer.draw_text(choice, cx - size.x*0.5f + o.x, baseY + o.y, 0.05f, glow2, false, true);
                }
                const glm::vec2 offs3[8] = {
                    { 3*px,  0}, {-3*px,  0}, {0,  3*py}, { 0, -3*py},
                    { 3*px, 3*py}, {-3*px, 3*py}, {3*px, -3*py}, {-3*px, -3*py}
                };
                for(const auto& o : offs3) {
                    renderer.draw_text(choice, cx - size.x*0.5f + o.x, baseY + o.y, 0.05f, glow3, false, true);
                }
            }
            renderer.draw_text(choice, x, 0.62f, 0.05f, glm::vec4(1.0), false, true);
            x += size.x + gap;
        }

        // Buttons row
        xmb->render_controller_buttons(renderer, 0.5f, 0.9f, std::array{
            std::pair{action::ok, "Enter"},
            std::pair{cancelable ? action::cancel : action::none, "Back"}
        });
    }

    result message_overlay::on_action(action action) {
        switch(action) {
            case action::cancel:
                if(cancelable) {
                    if(cancel_callback) {
                        cancel_callback();
                    }
                    return result::success | result::close | result::cancel_sound;
                }
                return result::unsupported;
            case action::ok:
                if(confirm_callback) {
                    confirm_callback(selected);
                }
                return result::success | result::close | result::confirm_sound;
            case action::left:
                if(selected > 0) {
                    selected--;
                    return result::success | result::ok_sound;
                }
                return result::unsupported | result::error_rumble;
            case action::right:
                if(selected < choices.size() - 1) {
                    selected++;
                    return result::success | result::ok_sound;
                }
                return result::unsupported | result::error_rumble;
            default:
                return result::unsupported;
        }
    }
}
