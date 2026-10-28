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

#include <functional>
#include <string>
#include <chrono>
#include <vector>

export module openxmb.app:message_overlay;

import auroreengine;
import sdl2;
import vulkan_hpp;
import vma;
import openxmb.utils;
import :component;

namespace app {

export class message_overlay : public component, public action_receiver {
    public:
        message_overlay(std::string title, std::string message, std::vector<std::string> choices = {},
            std::function<void(unsigned int)> confirm_callback = [](unsigned int){},
            bool cancelable = true, std::function<void()> cancel_callback = [](){}
        );
        // Turns the message into the PS3 name prompt: the title is the prompt,
        // a focusable field opens the on-screen keyboard and OK confirms.
        void set_text_field(std::string keyboard_title, std::string value, std::function<void(std::string)> accept);
        // Make message overlays opaque so underlying UI is hidden while active
        [[nodiscard]] bool is_opaque() const override { return true; }
        // Enable fade-in/out transitions so shell can animate background zoom/dim
        [[nodiscard]] bool do_fade_in() const override { return true; }
        [[nodiscard]] bool do_fade_out() const override { return true; }
        [[nodiscard]] bool staged_entrance() const override { return true; }
        [[nodiscard]] float fade_zoom() const override { return 1.0F; }
        [[nodiscard]] std::chrono::milliseconds fade_duration() const override;
        void render(auroreengine::gui_renderer& renderer, class shell* xmb) override;
        result tick(class shell* xmb) override;
        result on_action(action action) override;
    private:
        std::string title;
        std::string message;
        std::vector<std::string> choices;
        std::function<void(unsigned int)> confirm_callback;
        bool cancelable;
        std::function<void()> cancel_callback;

        bool has_field = false;
        std::string keyboard_title, field_value;
        std::function<void(std::string)> field_accept;

        // Focused button index; field_focused takes precedence when a field exists.
        unsigned int selected = 0;
        bool field_focused = false;
        bool pending = false;
        std::chrono::steady_clock::time_point pressed_at{};
        std::size_t scroll_line = 0, line_count = 0, visible_lines = 1;
        float wrapped_pixels = -1;
        std::string wrapped_font;
        std::vector<std::string> title_lines, body_lines;
        using time_point = std::chrono::time_point<std::chrono::steady_clock>;
        time_point start_time { std::chrono::steady_clock::now() };
};

}
