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

#include "openxmb/xmb/catalog.hpp"
#include "openxmb/xmb/settings_actions.hpp"
#include "openxmb/xmb/settings_controller.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

export module openxmb.app:main_menu;

import :menu_base;
import auroreengine;
import sdl2;
import vulkan_hpp;
import vma;
import openxmb.utils;
import openxmb.xmb.settings_scene_renderer;

namespace app {

class main_menu : public action_receiver {
    public:
        main_menu(class shell* xmb);
        void preload(vk::Device device, vma::Allocator allocator, auroreengine::resource_loader& loader);
        void render(auroreengine::gui_renderer& renderer);
        [[nodiscard]] float background_blur_px() const;
        [[nodiscard]] float background_dim_alpha() const;

        result on_action(action action) override;
    private:
        using time_point = std::chrono::time_point<std::chrono::steady_clock>;

        class shell* xmb;

        void render_crossbar(auroreengine::gui_renderer& renderer, time_point now);
        void render_submenu(auroreengine::gui_renderer& renderer, time_point now);

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
        bool settings_category_active() const;
        bool settings_chrome_collapsed() const;
        bool select_settings_relative(direction dir);
        bool activate_settings(action action);
        bool back_settings();
        bool open_settings_choice_overlay(const openxmb::xmb::CatalogNode& node);
        bool open_settings_dialog_choice_overlay(
            const openxmb::xmb::SettingsActionPlan& plan);
        void seed_settings_dialog_values();
        void apply_initial_settings_route();
        void render_settings_parent_layer(auroreengine::gui_renderer& renderer);
        void render_settings_scene(auroreengine::gui_renderer& renderer, time_point now);

        std::vector<std::unique_ptr<menu::menu>> menus;
        int selected = 0;

        int last_selected = 0;
        time_point last_selected_transition;
        constexpr static auto transition_duration = std::chrono::milliseconds(250);

        int last_selected_menu_item = 0;
        time_point last_selected_menu_item_transition;
        constexpr static auto transition_menu_item_duration = std::chrono::milliseconds(200);

        bool in_submenu = false;
        menu::menu* current_submenu = nullptr;
        std::vector<menu::menu*> submenu_stack;
        time_point last_submenu_transition;
        constexpr static auto transition_submenu_activate_duration = std::chrono::milliseconds(250);

        int last_selected_submenu_item = 0;
        time_point last_selected_submenu_item_transition;
        constexpr static auto transition_submenu_item_duration = std::chrono::milliseconds(200);

        struct settings_icon_texture {
            std::string semantic_id;
            std::unique_ptr<auroreengine::texture> texture;
            std::unique_ptr<auroreengine::texture> glass_texture;
        };

        std::optional<openxmb::xmb::Catalog> settings_catalog;
        std::optional<openxmb::xmb::SettingsCatalogController> settings_controller;
        openxmb::xmb::SettingsSceneRenderer settings_scene_renderer;
        std::vector<settings_icon_texture> settings_icon_textures;
        std::unordered_map<std::string, std::string> settings_value_labels;
        std::vector<openxmb::xmb::SettingsValueOverride> settings_render_values;
        std::vector<openxmb::xmb::SettingsSceneIcon> settings_render_icons;
        std::optional<openxmb::xmb::NavigationRoute> settings_previous_route;
        time_point settings_route_changed;
        bool settings_returning = false;
        bool settings_choice_panel_open = false;
};

}
