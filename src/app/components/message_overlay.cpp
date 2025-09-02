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
                // Glow dimensions follow the text tightly (pill shape)
                constexpr float height_scale = 0.72f; // thinner than text height
                glm::vec2 gsize = {size.x, size.y*height_scale};
                glm::vec2 center = {x + size.x/2.0f, 0.62f};
                glm::vec2 pos = {center.x - gsize.x/2.0f, center.y - gsize.y/2.0f};

                // Two‑layer glow: wide faint halo + tighter core
                float a1 = 0.05f + 0.05f * pulse;
                float a2 = 0.09f + 0.07f * pulse;

                auto draw_glow = [&](glm::vec2 p, glm::vec2 s, float alpha){
                    dreamrender::simple_renderer::params params = {
                        .blur = {
                            glm::vec2{-0.02f, 0.18f}/static_cast<float>(renderer.aspect_ratio), glm::vec2{-0.02f, 0.18f}/static_cast<float>(renderer.aspect_ratio),
                            glm::vec2{-0.10f, 0.35f}, glm::vec2{-0.10f, 0.35f}
                        },
                        .border_radius = {0.50f, 0.50f, 0.50f, 0.50f},
                        .aspect_ratio = static_cast<float>((s.x/s.y) * renderer.aspect_ratio)
                    };
                    renderer.draw_rect(p, s, glm::vec4(1.0f, 1.0f, 1.0f, alpha), params);
                };
                // Outer halo (slightly larger)
                draw_glow(pos - glm::vec2(0.010f/renderer.aspect_ratio, 0.006f), gsize + glm::vec2(0.020f/renderer.aspect_ratio, 0.012f), a1);
                // Core glow
                draw_glow(pos, gsize, a2);
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
                    return result::success | result::close;
                }
                return result::unsupported;
            case action::ok:
                if(confirm_callback) {
                    confirm_callback(selected);
                }
                return result::success | result::close;
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
