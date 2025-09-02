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

#include <chrono>
#include <functional>
#include <string>
#include <vector>

export module openxmb.app:choice_overlay;

import dreamrender;
import sdl2;
import vulkan_hpp;
import vma;
import openxmb.utils;
import :component;
import glm;

namespace app {

export class choice_overlay : public component, public action_receiver {
    public:
        choice_overlay(std::vector<std::string> choices, unsigned int selection_index = 0,
            std::function<void(unsigned int)> confirm_callback = [](unsigned int){},
            std::function<void()> cancel_callback = [](){}
        );
        // Optional colour swatches to display next to each choice (same length as choices or empty)
        void set_colour_swatches(const std::vector<glm::vec3>& cols) { swatches = cols; }

        void render(dreamrender::gui_renderer& renderer, class shell* xmb) override;
        result on_action(action action) override;

        [[nodiscard]] bool is_opaque() const override { return false; }
        [[nodiscard]] bool do_fade_in() const override { return true; }
        [[nodiscard]] bool do_fade_out() const override { return true; }
    private:
        using time_point = std::chrono::time_point<std::chrono::system_clock>;

        std::vector<std::string> choices;
        std::function<void(unsigned int)> confirm_callback;
        std::function<void()> cancel_callback;

        bool select_relative(action dir);

        unsigned int selection_index = 0;
        unsigned int last_selection_index = 0;
        time_point last_selection_time = std::chrono::system_clock::now();

        constexpr static auto transition_duration = std::chrono::milliseconds(100);

        std::vector<glm::vec3> swatches;
};

}
