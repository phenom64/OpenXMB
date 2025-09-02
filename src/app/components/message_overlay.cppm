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

export module openxmb.app:message_overlay;

import dreamrender;
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

        void render(dreamrender::gui_renderer& renderer, class shell* xmb) override;
        result on_action(action action) override;
    private:
        std::string title;
        std::string message;
        std::vector<std::string> choices;
        std::function<void(unsigned int)> confirm_callback;
        bool cancelable;
        std::function<void()> cancel_callback;

        unsigned int selected = 0;
};

}
