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

#include "openxmb/xmb/catalog.hpp"
#include "openxmb/xmb/settings_actions.hpp"
#include "openxmb/xmb/settings_controller.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

module openxmb.app;

import sdl2;
import spdlog;
import i18n;
import glm;
import vulkan_hpp;
import vma;
import dreamrender;
import openxmb.xmb.root_scene;
import openxmb.xmb.settings_scene_renderer;

import openxmb.config;
import :menu_base;
import :menu_utils;
import :choice_overlay;
import :applications_menu;
import :settings_menu;
import :users_menu;
import :files_menu;

using namespace mfk::i18n::literals;

namespace app {

namespace {

using namespace openxmb::xmb;

inline constexpr double logical_width = 1920.0;
inline constexpr double logical_height = 1080.0;
inline constexpr int settings_category_index = 1;
inline constexpr std::string_view initial_category_env = "OPENXMB_INITIAL_CATEGORY";
inline constexpr std::string_view initial_settings_menu_env = "OPENXMB_INITIAL_SETTINGS_MENU";
inline constexpr std::string_view initial_settings_selection_env =
    "OPENXMB_INITIAL_SETTINGS_SELECTION";
inline constexpr std::string_view initial_settings_open_choice_env =
    "OPENXMB_OPEN_INITIAL_SETTINGS_CHOICE";

struct contained_layout {
    double scale{};
    double offset_x{};
    double offset_y{};
    double framebuffer_width{};
    double framebuffer_height{};

    [[nodiscard]] float x(double logical_x) const noexcept {
        return static_cast<float>((offset_x + logical_x * scale) / framebuffer_width);
    }

    [[nodiscard]] float y(double logical_y) const noexcept {
        return static_cast<float>((offset_y + logical_y * scale) / framebuffer_height);
    }

    [[nodiscard]] float width(double logical_width) const noexcept {
        // ImageRenderer uses framebuffer-height units on both axes and applies
        // aspect correction internally. Dividing by width shrinks 16:9 icons.
        return static_cast<float>(logical_width * scale / framebuffer_height);
    }

    [[nodiscard]] float height(double logical_height) const noexcept {
        return static_cast<float>(logical_height * scale / framebuffer_height);
    }
};

[[nodiscard]] contained_layout make_contained_layout(const dreamrender::gui_renderer& renderer) noexcept {
    const auto width = static_cast<double>(renderer.frame_size.width);
    const auto height = static_cast<double>(renderer.frame_size.height);
    const auto scale = std::min(width / logical_width, height / logical_height);
    return {
        .scale = scale,
        .offset_x = (width - logical_width * scale) * 0.5,
        .offset_y = (height - logical_height * scale) * 0.5,
        .framebuffer_width = width,
        .framebuffer_height = height,
    };
}

[[nodiscard]] double ease_out_cubic(double value) noexcept {
    const auto t = std::clamp(value, 0.0, 1.0);
    const auto inverse = 1.0 - t;
    return 1.0 - inverse * inverse * inverse;
}

[[nodiscard]] double progress(std::chrono::steady_clock::time_point now,
                              std::chrono::steady_clock::time_point started,
                              std::chrono::milliseconds duration) noexcept {
    if(started.time_since_epoch().count() == 0) {
        return 1.0;
    }
    const auto elapsed_seconds = std::chrono::duration<double>(now - started).count();
    const auto duration_seconds = std::chrono::duration<double>(duration).count();
    return std::clamp(elapsed_seconds / duration_seconds, 0.0, 1.0);
}

[[nodiscard]] std::filesystem::path preferred_icon(
    const std::filesystem::path& asset_directory,
    std::string_view compatibility_name,
    std::string_view clean_name
) {
    const auto compatibility = asset_directory / "compat/xmb-ui-compat/icons" / compatibility_name;
    std::error_code error;
    if(std::filesystem::is_regular_file(compatibility, error) && !error) {
        return compatibility;
    }

    const auto clean = asset_directory / "icons" / clean_name;
    spdlog::debug("Compatibility icon {} is unavailable; using clean icon {}",
        compatibility.string(), clean.string());
    return clean;
}

[[nodiscard]] bool file_exists(const std::filesystem::path& path) noexcept {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

[[nodiscard]] std::optional<std::filesystem::path> resolve_compat_icon_ref(
    const std::filesystem::path& asset_directory,
    std::string_view icon_ref
) {
    constexpr std::string_view xmb_icon_prefix = "compat.icon.xmb.icon.";
    constexpr std::string_view compat_icon_prefix = "compat.icon.";
    const auto icon_directory = asset_directory / "compat/xmb-ui-compat/icons";

    if(icon_ref.starts_with(xmb_icon_prefix)) {
        const auto suffix = icon_ref.substr(xmb_icon_prefix.size());
        const auto filename = suffix == "psn"
            ? std::string{"xmb_icon_psn.png"}
            : std::string{"xmb_icon_"} + std::string{suffix} + ".png";
        const auto path = icon_directory / filename;
        if(file_exists(path)) {
            return path;
        }
    }

    if(icon_ref.starts_with(compat_icon_prefix)) {
        auto name = std::string{icon_ref.substr(compat_icon_prefix.size())};
        std::ranges::replace(name, '.', '_');
        const auto underscored = icon_directory / (name + ".png");
        if(file_exists(underscored)) {
            return underscored;
        }
        std::ranges::replace(name, '_', '-');
        const auto dashed = icon_directory / (name + ".png");
        if(file_exists(dashed)) {
            return dashed;
        }
    }

    return std::nullopt;
}

struct ps3_colour_entry {
    std::string_view value;
    glm::vec3 tint;
    glm::vec3 swatch;
};

[[nodiscard]] const std::array<ps3_colour_entry, 21>& ps3_colour_palette() {
    static const std::array<ps3_colour_entry, 21> colours{{
        {"original", {0.820F, 0.620F, 0.900F}, {0.720F, 0.750F, 0.790F}},
        {"yellow", {1.000F, 0.880F, 0.200F}, {0.800F, 0.720F, 0.110F}},
        {"green", {0.650F, 0.870F, 0.300F}, {0.450F, 0.660F, 0.160F}},
        {"pink", {1.000F, 0.640F, 0.720F}, {0.870F, 0.430F, 0.510F}},
        {"dark.green", {0.250F, 0.700F, 0.250F}, {0.110F, 0.480F, 0.110F}},
        {"light.purple", {0.820F, 0.620F, 0.900F}, {0.570F, 0.440F, 0.740F}},
        {"teal", {0.300F, 0.880F, 0.850F}, {0.140F, 0.720F, 0.670F}},
        {"dark.blue", {0.100F, 0.300F, 0.800F}, {0.030F, 0.220F, 0.650F}},
        {"magenta", {0.700F, 0.300F, 0.800F}, {0.550F, 0.210F, 0.610F}},
        {"orange", {1.000F, 0.700F, 0.150F}, {0.760F, 0.560F, 0.070F}},
        {"brown", {0.620F, 0.430F, 0.180F}, {0.620F, 0.430F, 0.180F}},
        {"red", {0.900F, 0.220F, 0.220F}, {0.900F, 0.220F, 0.220F}},
        {"black", {0.060F, 0.060F, 0.075F}, {0.040F, 0.040F, 0.047F}},
        {"white", {0.950F, 0.950F, 0.980F}, {0.950F, 0.950F, 0.960F}},
        {"gray", {0.550F, 0.570F, 0.620F}, {0.550F, 0.560F, 0.620F}},
        {"blue", {0.200F, 0.450F, 0.950F}, {0.210F, 0.450F, 0.950F}},
        {"cyan", {0.200F, 0.850F, 0.950F}, {0.200F, 0.850F, 0.950F}},
        {"lime", {0.550F, 0.950F, 0.200F}, {0.550F, 0.950F, 0.200F}},
        {"gold", {1.000F, 0.780F, 0.250F}, {1.000F, 0.780F, 0.250F}},
        {"violet", {0.550F, 0.350F, 0.950F}, {0.550F, 0.350F, 0.950F}},
        {"crimson", {0.800F, 0.100F, 0.300F}, {0.800F, 0.100F, 0.300F}},
    }};
    return colours;
}

[[nodiscard]] std::optional<ps3_colour_entry> ps3_colour_for_value(
    std::string_view value) {
    for(const auto& colour : ps3_colour_palette()) {
        if(colour.value == value) {
            return colour;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool near_colour(glm::vec3 lhs, glm::vec3 rhs) noexcept {
    constexpr float tolerance = 0.006F;
    return std::abs(lhs.r - rhs.r) <= tolerance &&
           std::abs(lhs.g - rhs.g) <= tolerance &&
           std::abs(lhs.b - rhs.b) <= tolerance;
}

[[nodiscard]] std::string_view current_ps3_colour_value() noexcept {
    if(config::CONFIG.themeOriginalColour) {
        return "original";
    }

    for(const auto& colour : ps3_colour_palette()) {
        if(colour.value == "original") {
            continue;
        }
        if(near_colour(config::CONFIG.themeCustomColour, colour.tint)) {
            return colour.value;
        }
    }
    return "light.purple";
}

[[nodiscard]] std::string_view current_ps3_background_value() noexcept {
    using background_type = config::config::background_type;
    switch(config::CONFIG.backgroundType) {
        case background_type::original:
            return "original";
        case background_type::wave:
            return "classic";
        case background_type::image:
            return "wallpaper";
        case background_type::color:
            return "brightness";
    }
    return "original";
}

[[nodiscard]] std::string_view current_ps3_theme_value() noexcept {
    return config::CONFIG.backgroundType == config::config::background_type::wave
        ? "classic"
        : "original";
}

[[nodiscard]] std::string_view current_ps3_font_value() noexcept {
    // The native font picker is not yet backed by packaged xmb-web font
    // variants. Keep the panel enterable and truthful instead of guessing.
    return "original";
}

[[nodiscard]] std::string_view current_ps3_day_night_value() noexcept {
    using day_night_mode = config::config::day_night_mode;
    switch(config::CONFIG.themeDayNightMode) {
        case day_night_mode::auto_time_of_day:
            return "auto.time.of.day";
        case day_night_mode::day:
            return "day";
        case day_night_mode::morning:
            return "morning";
        case day_night_mode::dusk:
            return "dusk";
        case day_night_mode::evening:
            return "evening";
        case day_night_mode::night:
            return "night";
    }
    return "auto.time.of.day";
}

[[nodiscard]] std::string_view current_dialog_value(std::string_view dialog_id) noexcept {
    if(dialog_id == "dialog.colour") {
        return current_ps3_colour_value();
    }
    if(dialog_id == "dialog.background") {
        return current_ps3_background_value();
    }
    if(dialog_id == "dialog.theme") {
        return current_ps3_theme_value();
    }
    if(dialog_id == "dialog.font") {
        return current_ps3_font_value();
    }
    if(dialog_id == "dialog.day.night") {
        return current_ps3_day_night_value();
    }
    return "";
}

[[nodiscard]] bool config_override_read_only() noexcept {
    const auto* raw = std::getenv("OPENXMB_CONFIG_READ_ONLY");
    if(raw == nullptr) {
        return false;
    }
    const std::string_view value{raw};
    return value == "1" || value == "true" || value == "TRUE" ||
           value == "yes" || value == "YES" || value == "on" || value == "ON";
}

[[nodiscard]] bool apply_settings_dialog_choice_value(
    std::string_view dialog_id,
    std::string_view choice_value,
    bool log_unsupported = true
) {
    if(dialog_id == "dialog.colour") {
        if(choice_value == "original") {
            config::CONFIG.setThemeOriginalColour(true);
            return true;
        }
        if(const auto colour = ps3_colour_for_value(choice_value)) {
            config::CONFIG.setThemeOriginalColour(false);
            config::CONFIG.setThemeCustomColour(colour->tint);
            return true;
        }
        return false;
    }

    if(dialog_id == "dialog.background") {
        using background_type = config::config::background_type;
        if(choice_value == "original") {
            config::CONFIG.setBackgroundType(background_type::original);
            return true;
        }
        if(choice_value == "classic") {
            config::CONFIG.setBackgroundType(background_type::wave);
            return true;
        }
        if(choice_value == "wallpaper") {
            if(!config::CONFIG.backgroundImage.empty()) {
                config::CONFIG.setBackgroundType(background_type::image);
                return true;
            }
            if(log_unsupported) {
                spdlog::info("Background Wallpaper is available only after a wallpaper path is configured");
            }
            return false;
        }
        if(choice_value == "brightness") {
            config::CONFIG.setBackgroundType(background_type::color);
            if(log_unsupported) {
                spdlog::info("Background Brightness opens a simplified native static-colour fallback until the full xmb-web brightness submenu is ported");
            }
            return true;
        }
        return false;
    }

    if(dialog_id == "dialog.theme") {
        using background_type = config::config::background_type;
        if(choice_value == "original") {
            config::CONFIG.setBackgroundType(background_type::original);
            return true;
        }
        if(choice_value == "classic") {
            config::CONFIG.setBackgroundType(background_type::wave);
            return true;
        }
        if(choice_value == "install" && log_unsupported) {
            spdlog::info("Theme Install is catalog-backed but not live-wired yet");
        }
        return false;
    }

    if(dialog_id == "dialog.font") {
        if(choice_value == "original") {
            config::CONFIG.setFontPath("default");
            return true;
        }
        if(log_unsupported) {
            spdlog::info("Font variant '{}' is catalog-backed but not packaged yet",
                choice_value);
        }
        return false;
    }

    if(dialog_id == "dialog.day.night") {
        config::CONFIG.setThemeDayNightMode(choice_value);
        return true;
    }

    return false;
}

[[nodiscard]] double seconds_from_time_point(
    std::chrono::steady_clock::time_point now) noexcept {
    return std::chrono::duration<double>(now.time_since_epoch()).count();
}

[[nodiscard]] std::optional<int> initial_category_index_from_env() noexcept {
    const auto* raw = std::getenv(initial_category_env.data());
    if(raw == nullptr) {
        if(std::getenv(initial_settings_menu_env.data()) != nullptr) {
            return settings_category_index;
        }
        return std::nullopt;
    }

    const std::string_view value{raw};
    if(value.empty()) {
        return std::nullopt;
    }

    constexpr std::array aliases{
        std::pair{std::string_view{"0"}, 0},
        std::pair{std::string_view{"users"}, 0},
        std::pair{std::string_view{"category.users"}, 0},
        std::pair{std::string_view{"1"}, 1},
        std::pair{std::string_view{"settings"}, 1},
        std::pair{std::string_view{"category.settings"}, 1},
        std::pair{std::string_view{"2"}, 2},
        std::pair{std::string_view{"photo"}, 2},
        std::pair{std::string_view{"category.photo"}, 2},
        std::pair{std::string_view{"3"}, 3},
        std::pair{std::string_view{"music"}, 3},
        std::pair{std::string_view{"category.music"}, 3},
        std::pair{std::string_view{"4"}, 4},
        std::pair{std::string_view{"video"}, 4},
        std::pair{std::string_view{"category.video"}, 4},
        std::pair{std::string_view{"5"}, 5},
        std::pair{std::string_view{"game"}, 5},
        std::pair{std::string_view{"category.game"}, 5},
        std::pair{std::string_view{"6"}, 6},
        std::pair{std::string_view{"network"}, 6},
        std::pair{std::string_view{"category.network"}, 6},
        std::pair{std::string_view{"7"}, 7},
        std::pair{std::string_view{"psn"}, 7},
        std::pair{std::string_view{"online"}, 7},
        std::pair{std::string_view{"playstation.network"}, 7},
        std::pair{std::string_view{"category.online"}, 7},
        std::pair{std::string_view{"8"}, 8},
        std::pair{std::string_view{"friends"}, 8},
        std::pair{std::string_view{"category.friends"}, 8},
    };

    for(const auto& [alias, index] : aliases) {
        if(value == alias) {
            return index;
        }
    }

    return -1;
}

[[nodiscard]] std::optional<std::string_view> env_string(std::string_view name) noexcept {
    const auto* raw = std::getenv(name.data());
    if(raw == nullptr || std::string_view{raw}.empty()) {
        return std::nullopt;
    }
    return std::string_view{raw};
}

[[nodiscard]] std::optional<std::size_t> env_size(std::string_view name) noexcept {
    const auto raw = env_string(name);
    if(!raw) {
        return std::nullopt;
    }
    std::size_t value{};
    const auto* begin = raw->data();
    const auto* end = begin + raw->size();
    const auto parsed = std::from_chars(begin, end, value);
    if(parsed.ec != std::errc{} || parsed.ptr != end) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool env_truthy(std::string_view name) noexcept {
    const auto raw = env_string(name);
    if(!raw) {
        return false;
    }
    return *raw == "1" || *raw == "true" || *raw == "yes" || *raw == "on";
}

[[nodiscard]] bool find_settings_menu_path(
    const openxmb::xmb::Catalog& catalog,
    std::string_view current_menu_id,
    std::string_view target_menu_id,
    std::vector<std::size_t>& path
) {
    if(current_menu_id == target_menu_id) {
        return true;
    }

    const auto* menu = catalog.find_node(current_menu_id);
    if(!menu) {
        return false;
    }

    for(std::size_t index = 0; index < menu->children.size(); ++index) {
        const auto* child = catalog.find_node(menu->children[index]);
        if(!child || child->children.empty()) {
            continue;
        }
        path.push_back(index);
        if(find_settings_menu_path(catalog, child->id, target_menu_id, path)) {
            return true;
        }
        path.pop_back();
    }

    return false;
}

void draw_icon(
    dreamrender::gui_renderer& renderer,
    const dreamrender::texture& texture,
    const dreamrender::texture* glass_texture,
    const contained_layout& layout,
    double center_x,
    double center_y,
    double extent,
    double alpha
) {
    if(alpha <= 0.001 || extent <= 0.0) {
        return;
    }

    const auto x = layout.x(center_x - extent * 0.5);
    const auto y = layout.y(center_y - extent * 0.5);
    const auto width = layout.width(extent);
    const auto height = layout.height(extent);
    const auto tint =
        glm::vec4(1.0f, 1.0f, 1.0f, static_cast<float>(alpha));
    if(config::CONFIG.iconGlassRefraction && glass_texture != nullptr && glass_texture->loaded) {
        renderer.draw_image_glass(*glass_texture, x, y, width, height, tint);
    } else {
        const auto flat_alpha =
            extent >= kActiveItemIconExtent - 0.5 ? alpha * 0.58 : alpha;
        const auto flat_tint =
            glm::vec4(1.0f, 1.0f, 1.0f, static_cast<float>(flat_alpha));
        renderer.draw_image_a(texture, x, y, width, height, flat_tint);
    }
}

void draw_normalized_icon(
    dreamrender::gui_renderer& renderer,
    const dreamrender::texture& texture,
    const dreamrender::texture* glass_texture,
    float x,
    float y,
    double width,
    double height,
    glm::vec4 tint = glm::vec4{1.0F}
) {
    if(config::CONFIG.iconGlassRefraction && glass_texture != nullptr && glass_texture->loaded) {
        renderer.draw_image_glass(*glass_texture, x, y,
            static_cast<float>(width), static_cast<float>(height), tint);
    } else {
        renderer.draw_image_a(texture, x, y,
            static_cast<float>(width), static_cast<float>(height), tint);
    }
}

[[nodiscard]] double item_slot_y(int index, int selected_index) noexcept {
    if(index == selected_index) {
        return kItemFocusY;
    }
    if(index < selected_index) {
        constexpr double item_above_base_y = 120.0;
        return item_above_base_y -
            static_cast<double>(selected_index - index - 1) * kItemPitch;
    }
    return kItemFocusY + kSelectedToNextPadding +
        static_cast<double>(index - selected_index) * kItemPitch;
}

} // namespace

main_menu::main_menu(app::shell* xmb) : xmb(xmb) {

}

void main_menu::preload(vk::Device device, vma::Allocator allocator, dreamrender::resource_loader& loader) {
    using ::menu::make_simple;
    using ::menu::make_simple_of;

    const auto& asset_directory = config::CONFIG.asset_directory;
    menus.reserve(9);
    menus.push_back(make_simple<menu::users_menu>("Users"_(),
        preferred_icon(asset_directory, "xmb_icon_000.png", "icon_category_users.png"), loader, xmb, loader));
    menus.push_back(make_simple<menu::settings_menu>("Settings"_(),
        preferred_icon(asset_directory, "xmb_icon_001.png", "icon_category_settings.png"), loader, xmb, loader));
    menus.push_back(make_simple<menu::files_menu>("Photo"_(),
        preferred_icon(asset_directory, "xmb_icon_002.png", "icon_category_photo.png"), loader, xmb,
        config::CONFIG.picturesPath, loader));
    menus.push_back(make_simple<menu::files_menu>("Music"_(),
        preferred_icon(asset_directory, "xmb_icon_003.png", "icon_category_music.png"), loader, xmb,
        config::CONFIG.musicPath, loader));
    menus.push_back(make_simple<menu::files_menu>("Video"_(),
        preferred_icon(asset_directory, "xmb_icon_004.png", "icon_category_video.png"), loader, xmb,
        config::CONFIG.videosPath, loader));
    menus.push_back(make_simple<menu::applications_menu>("Game"_(),
        preferred_icon(asset_directory, "xmb_icon_005.png", "icon_category_game.png"), loader,
        xmb, loader, ::menu::categoryFilter("Game")));
    menus.push_back(make_simple_of<menu::menu>("Network"_(),
        preferred_icon(asset_directory, "xmb_icon_006.png", "icon_category_network.png"), loader));
    menus.push_back(make_simple_of<menu::menu>("PlayStation Network"_(),
        preferred_icon(asset_directory, "xmb_icon_psn.png", "icon_category_network.png"), loader));
    menus.push_back(make_simple_of<menu::menu>("Friends"_(),
        preferred_icon(asset_directory, "xmb_icon_007.png", "icon_category_friends.png"), loader));

    if(const auto initial_category = initial_category_index_from_env()) {
        if(*initial_category >= 0 &&
           static_cast<std::size_t>(*initial_category) < menus.size()) {
            selected = *initial_category;
            last_selected = selected;
            last_selected_menu_item = menus[static_cast<std::size_t>(selected)]
                ->get_selected_submenu();
        } else {
            spdlog::warn("Ignoring unsupported {} value", initial_category_env);
        }
    }

    menus[selected]->on_open();

    const auto catalog_path = asset_directory / "catalog/en.json";
    if(auto loaded = openxmb::xmb::load_catalog_file(catalog_path); loaded) {
        settings_catalog.emplace(std::move(*loaded.value));
        if(auto created = openxmb::xmb::SettingsCatalogController::create(*settings_catalog); created) {
            settings_controller.emplace(std::move(*created.controller));
            std::unordered_set<std::string> loaded_icon_refs;
            for(const auto& [node_id, node] : settings_catalog->nodes) {
                (void)node_id;
                if(node.icon_ref.empty() || !loaded_icon_refs.insert(node.icon_ref).second) {
                    continue;
                }
                auto texture = std::make_unique<dreamrender::texture>(device, allocator);
                const auto path = resolve_compat_icon_ref(asset_directory, node.icon_ref)
                    .value_or(asset_directory / "icons/icon_category_settings.png");
                try {
                    loader.loadTexture(texture.get(), path);
                    std::unique_ptr<dreamrender::texture> glass_texture;
                    if(const auto normal_map = menu::xmb_web_normal_map_for_icon(path)) {
                        glass_texture = std::make_unique<dreamrender::texture>(device, allocator);
                        loader.loadTexture(glass_texture.get(), *normal_map);
                    }
                    settings_icon_textures.push_back({
                        .semantic_id = node.icon_ref,
                        .texture = std::move(texture),
                        .glass_texture = std::move(glass_texture),
                    });
                } catch(const std::exception& error) {
                    spdlog::debug("Failed to queue Settings icon {} from {}: {}",
                        node.icon_ref, path.string(), error.what());
                }
            }
            spdlog::info("Loaded catalog-backed Settings controller with {} icon binding(s)",
                settings_icon_textures.size());
            apply_initial_settings_route();
        } else {
            spdlog::warn("Settings catalog loaded from {}, but controller creation failed",
                catalog_path.string());
        }
    } else if(loaded.error) {
        spdlog::warn("Could not load Settings catalog {}: {}",
            catalog_path.string(), loaded.error->message);
    } else {
        spdlog::warn("Could not load Settings catalog {}", catalog_path.string());
    }
}

result main_menu::on_action(action action) {
    switch(action) {
        case action::left:
            return select_relative(direction::left) ? result::success | result::ok_sound : result::unsupported | result::error_rumble;
        case action::right:
            return select_relative(direction::right) ? result::success | result::ok_sound : result::unsupported | result::error_rumble;
        case action::up:
            return select_relative(direction::up) ? result::success | result::ok_sound : result::unsupported | result::error_rumble;
        case action::down:
            return select_relative(direction::down) ? result::success | result::ok_sound : result::unsupported | result::error_rumble;
        case action::ok:
            return activate_current(action)
                ? result::success | result::confirm_sound
                : result::unsupported | result::error_rumble;
        case action::options:
        case action::extra:
            return activate_current(action)
                ? result::success | result::ok_sound
                : result::unsupported | result::error_rumble;
        case action::cancel:
            return back() ? (result::success | result::back_sound) : result::unsupported | result::error_rumble;
        default:
            return result::unsupported;
    }
    return result::unsupported;
}

bool main_menu::select_relative(direction dir) {
    if(!in_submenu) {
        if(settings_category_active() &&
           (dir == direction::up || dir == direction::down)) {
            return select_settings_relative(dir);
        }
        if(dir == direction::left) {
            if(selected > 0) {
                select(selected-1);
                return true;
            }
        } else if(dir == direction::right) {
            if(selected < menus.size()-1) {
                select(selected+1);
                return true;
            }
        } else if(dir == direction::up) {
            auto& menu = menus[selected];
            if(menu->get_selected_submenu() > 0) {
                select_menu_item(menu->get_selected_submenu()-1);
                return true;
            }
        } else if(dir == direction::down) {
            auto& menu = menus[selected];
            if(menu->get_selected_submenu() < menu->get_submenus_count()-1) {
                select_menu_item(menu->get_selected_submenu()+1);
                return true;
            }
        }
    } else {
        auto& menu = current_submenu;
        if(dir == direction::up) {
            if(menu->get_selected_submenu() > 0) {
                select_submenu_item(menu->get_selected_submenu()-1);
                return true;
            }
        } else if(dir == direction::down) {
            if(menu->get_selected_submenu() < menu->get_submenus_count()-1) {
                select_submenu_item(menu->get_selected_submenu()+1);
                return true;
            }
        }
    }
    return false;
}
bool main_menu::activate_current(action action) {
    if(settings_category_active()) {
        return activate_settings(action);
    }
    auto& menu = *menus[selected];
    auto res = menu.activate(action);
    if(res == result::submenu) {
        auto& entry = current_submenu ? current_submenu->get_submenu(current_submenu->get_selected_submenu())
            : menu.get_submenu(menu.get_selected_submenu());
        if(auto submenu = dynamic_cast<menu::menu*>(&entry)) {
            if(submenu->get_submenus_count() > 0) {
                if(current_submenu) {
                    submenu_stack.push_back(current_submenu);
                }

                current_submenu = submenu;
                current_submenu->on_open();

                if(!in_submenu) {
                    in_submenu = true;
                    last_submenu_transition = std::chrono::steady_clock::now();
                }
                return true;
            }
        }
        return false;
    }
    return res == result::success;
}
bool main_menu::back() {
    if(settings_category_active() && back_settings()) {
        return true;
    }
    if(in_submenu) {
        current_submenu->on_close();
        if(submenu_stack.empty()) {
            current_submenu = nullptr;
            in_submenu = false;
            last_submenu_transition = std::chrono::steady_clock::now();
        } else {
            current_submenu = submenu_stack.back();
            submenu_stack.pop_back();
        }
        return true;
    }
    return false;
}

bool main_menu::settings_category_active() const {
    return selected == settings_category_index && settings_catalog &&
        settings_controller && !in_submenu;
}

bool main_menu::select_settings_relative(direction dir) {
    if(!settings_controller) {
        return false;
    }
    const auto delta = dir == direction::up ? -1 : 1;
    const auto step = settings_controller->move_selection(
        delta, seconds_from_time_point(std::chrono::steady_clock::now()));
    return step && step.changed;
}

bool main_menu::activate_settings(action action) {
    if(action != action::ok || !settings_catalog || !settings_controller) {
        return false;
    }

    const auto& route = settings_controller->route();
    if(route.layer != openxmb::xmb::NavigationLayer::nested_menu) {
        return false;
    }
    const auto* menu_node = settings_catalog->find_node(route.id);
    if(!menu_node || route.selection >= menu_node->children.size()) {
        return false;
    }

    const auto resolved = openxmb::xmb::resolve_settings_action(
        *settings_catalog, menu_node->children[route.selection]);
    if(!resolved || !resolved.plan) {
        return false;
    }

    switch(resolved.plan->kind) {
        case openxmb::xmb::SettingsActionKind::navigate_menu: {
            const auto step = settings_controller->activate(
                seconds_from_time_point(std::chrono::steady_clock::now()));
            return step && step.changed;
        }
        case openxmb::xmb::SettingsActionKind::open_dialog:
            if(open_settings_dialog_choice_overlay(*resolved.plan)) {
                return true;
            }
            spdlog::info("Settings dialog '{}' is catalog-backed but has no live presenter yet",
                resolved.plan->target_id);
            return false;
        case openxmb::xmb::SettingsActionKind::simulated_setting:
            if(resolved.plan->may_update_settings_state) {
                if(const auto* node = settings_catalog->find_node(resolved.plan->node_id);
                   node && open_settings_choice_overlay(*node)) {
                    return true;
                }
            }
            spdlog::info("Settings '{}' is catalog-backed but has no live value panel yet",
                resolved.plan->node_id);
            return false;
        default:
            spdlog::info("Settings action '{}' resolves to '{}', but dialog/wizard presentation is not live-wired yet",
                resolved.plan->node_id, resolved.plan->target_id);
            return false;
    }
}

bool main_menu::back_settings() {
    if(!settings_controller || settings_controller->navigation().stack.size() <= 2) {
        return false;
    }
    const auto step = settings_controller->back(
        seconds_from_time_point(std::chrono::steady_clock::now()));
    return step && step.changed;
}

bool main_menu::open_settings_choice_overlay(const openxmb::xmb::CatalogNode& node) {
    if(!settings_catalog || node.choices.empty()) {
        return false;
    }

    std::vector<std::string> labels;
    labels.reserve(node.choices.size());
    for(const auto& choice : node.choices) {
        const auto localized = settings_catalog->text(choice.label_key);
        labels.push_back(localized.empty() ? choice.value : std::string{localized});
    }

    auto selection = static_cast<unsigned int>(
        std::min(node.default_selection, node.choices.size() - 1));
    if(const auto saved = settings_value_labels.find(node.id);
       saved != settings_value_labels.end()) {
        for(std::size_t index = 0; index < labels.size(); ++index) {
            if(labels[index] == saved->second) {
                selection = static_cast<unsigned int>(index);
                break;
            }
        }
    }

    xmb->emplace_overlay<app::choice_overlay>(
        labels,
        selection,
        [this, node_id = node.id, labels](unsigned int index) {
            if(index < labels.size()) {
                settings_value_labels[node_id] = labels[index];
            }
        });
    return true;
}

bool main_menu::open_settings_dialog_choice_overlay(
    const openxmb::xmb::SettingsActionPlan& plan) {
    if(!settings_catalog) {
        return false;
    }
    if(plan.target_id.rfind("dialog.", 0) != 0) {
        return false;
    }

    const auto dialog_it = settings_catalog->dialogs.find(plan.target_id);
    if(dialog_it == settings_catalog->dialogs.end() ||
       dialog_it->second.choices.empty()) {
        return false;
    }

    const auto& dialog = dialog_it->second;
    std::vector<openxmb::xmb::CatalogChoice> selectable_choices;
    selectable_choices.reserve(dialog.choices.size());
    std::string top_action_label;
    for(const auto& choice : dialog.choices) {
        const bool top_action =
            (plan.target_id == "dialog.theme" && choice.value == "install") ||
            (plan.target_id == "dialog.background" && choice.value == "brightness");
        if(top_action) {
            const auto localized = settings_catalog->text(choice.label_key);
            top_action_label =
                localized.empty() ? choice.value : std::string{localized};
            continue;
        }
        selectable_choices.push_back(choice);
    }
    if(selectable_choices.empty()) {
        return false;
    }

    std::vector<std::string> labels;
    labels.reserve(selectable_choices.size());
    for(const auto& choice : selectable_choices) {
        const auto localized = settings_catalog->text(choice.label_key);
        labels.push_back(localized.empty() ? choice.value : std::string{localized});
    }

    const auto current_value = current_dialog_value(plan.target_id);
    unsigned int selection = 0;
    if(!current_value.empty()) {
        for(std::size_t index = 0; index < selectable_choices.size(); ++index) {
            if(selectable_choices[index].value == current_value) {
                selection = static_cast<unsigned int>(index);
                break;
            }
        }
    }
    if(selection < labels.size()) {
        settings_value_labels[plan.node_id] = labels[selection];
    }

    std::vector<glm::vec3> swatches;
    if(plan.target_id == "dialog.colour") {
        swatches.reserve(selectable_choices.size());
        for(const auto& choice : selectable_choices) {
            const auto colour = ps3_colour_for_value(choice.value)
                .value_or(ps3_colour_entry{choice.value, {0.72F, 0.75F, 0.79F}, {0.72F, 0.75F, 0.79F}});
            swatches.push_back(colour.swatch);
        }
    }

    const auto dialog_id = plan.target_id;
    const auto node_id = plan.node_id;
    const auto original_selection = selection;
    auto* overlay = xmb->emplace_overlay<app::choice_overlay>(
        labels,
        selection,
        [this,
         dialog_id,
         node_id,
         labels,
         choices = selectable_choices](unsigned int index) {
            if(index >= choices.size() || index >= labels.size()) {
                return;
            }

            const auto& choice = choices[index];
            const bool applied = apply_settings_dialog_choice_value(
                dialog_id, choice.value, true);

            if(applied) {
                settings_value_labels[node_id] = labels[index];
            }

            if(applied) {
                if(config_override_read_only()) {
                    spdlog::info("Skipping config save because OPENXMB_CONFIG_READ_ONLY is set");
                } else {
                    config::CONFIG.save_config();
                }
            }
        },
        [this, dialog_id, node_id, labels, choices = selectable_choices,
         original_selection]() {
            if(original_selection < choices.size()) {
                (void)apply_settings_dialog_choice_value(
                    dialog_id, choices[original_selection].value, false);
            }
            if(original_selection < labels.size()) {
                settings_value_labels[node_id] = labels[original_selection];
            }
        },
        [dialog_id, choices = selectable_choices](unsigned int index) {
            if(index < choices.size()) {
                (void)apply_settings_dialog_choice_value(
                    dialog_id, choices[index].value, false);
            }
        });

    if(!swatches.empty()) {
        overlay->set_colour_swatches(swatches);
    }
    if(!top_action_label.empty()) {
        const float top_action_y = plan.target_id == "dialog.theme"
            ? 270.0F / 1080.0F
            : 390.0F / 1080.0F;
        overlay->set_top_action(top_action_label, top_action_y);
    }
    return true;
}

void main_menu::apply_initial_settings_route() {
    if(!settings_catalog || !settings_controller) {
        return;
    }

    const auto target_menu = env_string(initial_settings_menu_env);
    if(!target_menu) {
        return;
    }
    if(!openxmb::xmb::is_settings_catalog_id(*target_menu)) {
        spdlog::warn("Ignoring non-Settings {} value", initial_settings_menu_env);
        return;
    }

    selected = settings_category_index;
    last_selected = selected;
    last_selected_menu_item = menus[static_cast<std::size_t>(selected)]
        ->get_selected_submenu();

    std::vector<std::size_t> path;
    if(!find_settings_menu_path(
           *settings_catalog, "category.settings", *target_menu, path)) {
        spdlog::warn("Ignoring unknown {} value '{}'",
            initial_settings_menu_env, *target_menu);
        return;
    }

    const auto now = seconds_from_time_point(std::chrono::steady_clock::now());
    for(const auto selection : path) {
        const auto selected_child = settings_controller->set_selection(selection, now);
        if(!selected_child) {
            spdlog::warn("Could not select Settings route segment {} for '{}'",
                selection, *target_menu);
            return;
        }
        const auto entered = settings_controller->activate(now);
        if(!entered || !entered.changed) {
            spdlog::warn("Could not enter Settings route segment {} for '{}'",
                selection, *target_menu);
            return;
        }
    }

    if(const auto selection = env_size(initial_settings_selection_env)) {
        if(const auto selected_child = settings_controller->set_selection(*selection, now);
           !selected_child) {
            spdlog::warn("Ignoring unsupported {} value",
                initial_settings_selection_env);
        }
    }

    if(env_truthy(initial_settings_open_choice_env) &&
       !activate_settings(action::ok)) {
        spdlog::warn("Could not open initial Settings choice panel");
    }
}

void main_menu::select(int index) {
    if(index == selected) {
        return;
    }
    if(index < 0 || index >= menus.size()) {
        return;
    }

    last_selected = selected;
    last_selected_transition = std::chrono::steady_clock::now();
    selected = index;

    menus[last_selected]->on_close();
    menus[selected]->on_open();

    last_selected_menu_item = menus[selected]->get_selected_submenu();
}
void main_menu::select_menu_item(int index) {
    auto& menu = menus[selected];
    if(index == menu->get_selected_submenu()) {
        return;
    }
    if(index < 0 || index >= menu->get_submenus_count()) {
        return;
    }

    last_selected_menu_item = menu->get_selected_submenu();
    last_selected_menu_item_transition = std::chrono::steady_clock::now();
    menu->select_submenu(index);
}
void main_menu::select_submenu_item(int index) {
    auto& menu = current_submenu;
    if(index == menu->get_selected_submenu()) {
        return;
    }
    if(index < 0 || index >= menu->get_submenus_count()) {
        return;
    }

    last_selected_submenu_item = menu->get_selected_submenu();
    last_selected_submenu_item_transition = std::chrono::steady_clock::now();
    menu->select_submenu(index);
}

void main_menu::render(dreamrender::gui_renderer& renderer) {
    auto now = std::chrono::steady_clock::now();

    const auto submenu_progress = ease_out_cubic(
        progress(now, last_submenu_transition, transition_submenu_activate_duration));
    const auto partial = last_submenu_transition.time_since_epoch().count() == 0
        ? (in_submenu ? 1.0 : 0.0)
        : (in_submenu ? submenu_progress : 1.0 - submenu_progress);
    bool in_submenu_now = in_submenu || partial > 0.0;

    render_crossbar(renderer, now);
    if(settings_category_active()) {
        render_settings_scene(renderer, now);
    }

    std::vector<std::pair<action, std::string>> buttons{};
    buttons.reserve(5);
    menus[selected]->get_button_actions(buttons);
    if(in_submenu_now && current_submenu) {
        render_submenu(renderer, now);
        current_submenu->get_button_actions(buttons);
    }
    if(in_submenu_now) {
        xmb->render_controller_buttons(renderer, 0.5f, 0.9f, buttons);
    }
}

void main_menu::render_settings_scene(dreamrender::gui_renderer& renderer, time_point now) {
    if(!settings_catalog || !settings_controller) {
        return;
    }

    std::vector<openxmb::xmb::SettingsValueOverride> value_overrides;
    value_overrides.reserve(settings_value_labels.size());
    for(const auto& [node_id, label] : settings_value_labels) {
        value_overrides.push_back({
            .node_id = node_id,
            .localized_value = label,
        });
    }

    const auto sampled = settings_controller->sample_scene(
        seconds_from_time_point(now), value_overrides);
    if(!sampled) {
        return;
    }

    std::vector<openxmb::xmb::SettingsSceneIcon> icons;
    icons.reserve(settings_icon_textures.size());
    for(const auto& entry : settings_icon_textures) {
        icons.push_back({
            .semantic_id = entry.semantic_id,
            .texture = entry.texture.get(),
            .glass_texture = entry.glass_texture.get(),
        });
    }

    settings_scene_renderer.render(
        renderer,
        sampled.snapshot,
        icons,
        {.glass_icons = config::CONFIG.iconGlassRefraction});
}

void main_menu::render_crossbar(dreamrender::gui_renderer& renderer, time_point now) {
    const auto layout = make_contained_layout(renderer);

    const auto category_linear = selected == last_selected
        ? 1.0
        : progress(now, last_selected_transition, transition_duration);
    const auto category_eased = ease_out_cubic(category_linear);
    const auto visual_selection = static_cast<double>(last_selected) +
        static_cast<double>(selected - last_selected) * category_eased;

    const auto submenu_linear = progress(
        now, last_submenu_transition, transition_submenu_activate_duration);
    const auto submenu_eased = ease_out_cubic(submenu_linear);
    const auto submenu_transition = last_submenu_transition.time_since_epoch().count() == 0
        ? (in_submenu ? 1.0 : 0.0)
        : (in_submenu ? submenu_eased : 1.0 - submenu_eased);

    for(std::size_t index = 0; index < menus.size(); ++index) {
        const auto active = static_cast<int>(index) == selected;
        const auto distance = std::abs(static_cast<int>(index) - selected);
        auto center_x = kActiveCategoryX +
            (static_cast<double>(index) - visual_selection) * kCategorySpacing;
        auto center_y = kCategoryBaselineY + (active ? -10.0 : 0.0);
        auto extent = active ? kActiveCategoryIconExtent : kInactiveCategoryIconExtent;
        auto alpha = active ? 1.0 : (distance <= 2 ? 0.9 : 0.25);

        if(active) {
            center_x -= 257.0 * submenu_transition;
            extent *= 1.0 + (kInactiveItemScaleTier - 1.0) * submenu_transition;
            alpha *= 1.0 + (0.6 - 1.0) * submenu_transition;
        } else {
            alpha *= 1.0 - submenu_transition;
        }

        // The PSN sphere fills substantially more of its source canvas than
        // the numbered icon family. xmb-web measures a 0.68 correction.
        if(index == 7) {
            extent *= 0.68;
        }

        draw_icon(renderer, menus[index]->get_icon(),
            menus[index]->get_glass_icon(), layout,
            center_x, center_y, extent, alpha);

        if(active) {
            renderer.draw_text(
                menus[index]->get_name(),
                layout.x(center_x),
                layout.y(kCategoryLabelY),
                layout.height(kCategoryLabelSize * 2.5),
                glm::vec4(225.0f / 255.0f, 210.0f / 255.0f, 235.0f / 255.0f,
                    static_cast<float>(0.9 * (1.0 - 0.55 * submenu_transition))),
                true,
                false);
        }
    }

    const auto render_item_rail = [&](const menu::menu& menu,
                                      int selected_item,
                                      int previous_item,
                                      double transition,
                                      double x_shift,
                                      double alpha_multiplier) {
        const auto eased = ease_out_cubic(transition);
        const auto count = static_cast<int>(menu.get_submenus_count());
        for(int index = 0; index < count; ++index) {
            const auto old_y = item_slot_y(index, previous_item);
            const auto new_y = item_slot_y(index, selected_item);
            const auto center_y = old_y + (new_y - old_y) * eased;
            if(center_y < -80.0 || center_y > logical_height + 80.0) {
                continue;
            }

            const auto focused = index == selected_item;
            const auto extent = focused ? kActiveItemIconExtent : kInactiveItemIconExtent;
            const auto alpha = (focused ? kFocusAlpha : kInactiveItemAlpha) * alpha_multiplier;
            auto& entry = menu.get_submenu(static_cast<unsigned int>(index));
            draw_icon(renderer, entry.get_icon(), entry.get_glass_icon(), layout,
                kItemIconX + x_shift, center_y, extent, alpha);

            const auto label_size = focused ? kActiveItemLabelSize : kInactiveItemLabelSize;
            const auto text_value = focused ? 1.0f : 235.0f / 255.0f;
            renderer.draw_text(
                entry.get_name(),
                layout.x(kItemLabelX + x_shift),
                layout.y(center_y),
                layout.height(label_size * 2.5),
                glm::vec4(text_value, text_value, text_value, static_cast<float>(alpha)),
                false,
                true);
        }
    };

    const auto rail_alpha = std::max(0.0, 1.0 - submenu_transition);
    if(settings_category_active()) {
        return;
    }

    if(selected != last_selected && category_linear < 1.0) {
        const auto travel = static_cast<double>(selected - last_selected) * kCategorySpacing;
        const auto old_shift = -travel * category_eased;
        const auto new_shift = travel * (1.0 - category_eased);
        const auto old_alpha = std::max(0.0, 1.0 - category_eased / 0.4) * rail_alpha;
        const auto new_alpha = std::max(0.0, (category_eased - 0.5) / 0.5) * rail_alpha;

        const auto& old_menu = *menus[static_cast<std::size_t>(last_selected)];
        const auto old_selected = static_cast<int>(old_menu.get_selected_submenu());
        render_item_rail(old_menu, old_selected, old_selected, 1.0,
            old_shift, old_alpha);

        const auto& new_menu = *menus[static_cast<std::size_t>(selected)];
        const auto new_selected = static_cast<int>(new_menu.get_selected_submenu());
        render_item_rail(new_menu, new_selected, new_selected, 1.0,
            new_shift, new_alpha);
    } else {
        const auto& current = *menus[static_cast<std::size_t>(selected)];
        const auto selected_item = static_cast<int>(current.get_selected_submenu());
        const auto item_linear = selected_item == last_selected_menu_item
            ? 1.0
            : progress(now, last_selected_menu_item_transition, transition_menu_item_duration);
        render_item_rail(current, selected_item, last_selected_menu_item,
            item_linear, 0.0, rail_alpha);
        if(item_linear >= 1.0) {
            last_selected_menu_item = selected_item;
        }
    }
}

void main_menu::render_submenu(dreamrender::gui_renderer& renderer, time_point now) {
    double submenu_transition = std::clamp(
        std::chrono::duration<double>(now - last_submenu_transition) / transition_submenu_activate_duration, 0.0, 1.0);
    submenu_transition = in_submenu ? submenu_transition : 1.0 - submenu_transition;

    constexpr auto offset = (0.1f-0.075f)/2.0f;
    const glm::vec2 base_pos = glm::mix(
        glm::vec2(0.35f/renderer.aspect_ratio, 0.25f),
        glm::vec2((0.15f-offset)/renderer.aspect_ratio, 0.25f-2*offset),
        submenu_transition);
    const double base_size = 0.1;

    const auto& selected_menu = *menus[selected];
    const auto& selected_submenu = *current_submenu;

    draw_normalized_icon(renderer, selected_menu.get_icon(), selected_menu.get_glass_icon(),
        base_pos.x, base_pos.y, 0.1, 0.1);
    draw_normalized_icon(renderer, selected_submenu.get_icon(), selected_submenu.get_glass_icon(),
        base_pos.x, base_pos.y + 0.15F, 0.1, 0.1);

    if(!in_submenu)
        return;
    if(const auto* submenu = dynamic_cast<const menu::menu*>(&selected_submenu)) {
        double selected = submenu->get_selected_submenu();
        auto time_since_transition = std::chrono::duration<double>(now - last_selected_submenu_item_transition);
        if(time_since_transition < transition_submenu_item_duration) {
            selected = last_selected_submenu_item + (selected - last_selected_submenu_item) *
                time_since_transition / transition_submenu_item_duration;
        }

        double offsetY = 0.15f - selected*0.15f;

        for(int i=0; i<submenu->get_submenus_count(); i++) {
            double partial_selection = 0.0;
            if(i == selected) {
                partial_selection = std::clamp(time_since_transition / transition_submenu_item_duration, 0.0, 1.0);
            }

            double size = base_size*glm::mix(0.75, 1.0, partial_selection);
            double offset = (base_size - size) / 4.0;

            double y = base_pos.y+offsetY+0.15f*i;
            if(y < -size || y > 1.0+size)
                continue;

            auto& entry = submenu->get_submenu(i);
            draw_normalized_icon(renderer, entry.get_icon(), entry.get_glass_icon(),
                static_cast<float>(base_pos.x + 0.1 + offset), static_cast<float>(y), size, size);
            renderer.draw_text(entry.get_name(), base_pos.x + 0.2, y+size/2, size/2, glm::vec4(1, 1, 1, 1), false, true);
            if(i == selected) {
                auto s = renderer.measure_text(entry.get_name(), size/2);
                renderer.draw_text(entry.get_description(), base_pos.x + 0.2, y+size/2 + s.y, size / 3);
            }
        }
    }
}

}
