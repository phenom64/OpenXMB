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
#include <memory>
#include <vector>

export module openxmb.app:main_menu;

import :menu_base;
import dreamrender;
import sdl2;
import vulkan_hpp;
import vma;
import openxmb.utils;

namespace app {

class main_menu : public action_receiver {
    public:
        main_menu(class shell* xmb);
        void preload(vk::Device device, vma::Allocator allocator, dreamrender::resource_loader& loader);
        void render(dreamrender::gui_renderer& renderer);

        result on_action(action action) override;
    private:
        using time_point = std::chrono::time_point<std::chrono::system_clock>;

        class shell* xmb;

        void render_crossbar(dreamrender::gui_renderer& renderer, time_point now);
        void render_submenu(dreamrender::gui_renderer& renderer, time_point now);

        enum class direction {
            left,
            right,
            up,
            down
        };

        void select(int index);
        void select_menu_item(int index);
        void select_submenu_item(int index);
        bool select_relative(direction dir);
        bool activate_current(action action);
        bool back();

        std::vector<std::unique_ptr<menu::menu>> menus;
        int selected = 0;

        int last_selected = 0;
        time_point last_selected_transition;
        constexpr static auto transition_duration = std::chrono::milliseconds(200);

        int last_selected_menu_item = 0;
        time_point last_selected_menu_item_transition;
        constexpr static auto transition_menu_item_duration = std::chrono::milliseconds(200);

        bool in_submenu = false;
        menu::menu* current_submenu = nullptr;
        std::vector<menu::menu*> submenu_stack;
        time_point last_submenu_transition;
        constexpr static auto transition_submenu_activate_duration = std::chrono::milliseconds(100);

        int last_selected_submenu_item = 0;
        time_point last_selected_submenu_item_transition;
        constexpr static auto transition_submenu_item_duration = std::chrono::milliseconds(100);
};

}
