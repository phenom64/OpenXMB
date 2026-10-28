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
#include "../../localization/ui_literal_text.hpp"

#include "openxmb/xmb/catalog.hpp"
#include "desktop_settings_catalog.hpp"
#include "../../localization/shell_locale.hpp"
#include "media_motion.hpp"
#include "openxmb/xmb/background.hpp"
#include "openxmb/desktop/library.hpp"
#include "openxmb/themes/library.hpp"
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
import auroreengine;
import openxmb.xmb.root_scene;
import openxmb.xmb.settings_scene_renderer;

import openxmb.config;
import :menu_base;
import :menu_utils;
import :choice_overlay;
import :xmb_presentation;
import :applications_menu;
import :settings_menu;
import :users_menu;
import :files_menu;
import :desktop_settings;
import :message_overlay;
import :text_entry;

using namespace mfk::i18n::literals;

namespace app {

namespace {

using namespace openxmb::xmb;

inline constexpr double logical_width = 1920.0;
inline constexpr double logical_height = 1080.0;
inline constexpr int settings_category_index = 1;
inline constexpr float submenu_backdrop_blur_px = 9.0F;
inline constexpr float submenu_backdrop_dim_alpha = 0.0F;
inline constexpr double settings_parent_col_x = 316.0;
inline constexpr double settings_sibling_col_x = 455.0;
inline constexpr double settings_parent_spacing = 80.0;
inline constexpr double xmb_web_icon_glass_alpha = 1.0;
inline constexpr double xmb_web_flat_active_icon_alpha = 0.54;
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

[[nodiscard]] contained_layout make_contained_layout(const auroreengine::gui_renderer& renderer) noexcept {
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
    const auto bundled = asset_directory / "icons" / compatibility_name;
    std::error_code bundled_error;
    if(std::filesystem::is_regular_file(bundled, bundled_error) && !bundled_error) {
        return bundled;
    }
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

    constexpr std::array<std::pair<std::string_view,std::string_view>,17> settings_icons{{
        {"008","icon_update"},{"005","icon_game_setting"},{"009","icon_bdvd_setting"},
        {"003","icon_music_setting"},{"042","icon_chat_setting"},{"012","icon_system_setting"},
        {"023","icon_theme_setting"},{"014","icon_datetime_setting"},{"056","icon_powersave_setting"},
        {"015","icon_accessory"},{"010","icon_printer_setting"},{"016","icon_display_setting"},
        {"017","icon_sound_setting"},{"018","icon_security_setting"},{"020","icon_remoteplay_setting"},
        {"006","icon_network_setting"},{"022","icon_setting_item"}
    }};
    if(icon_ref.starts_with(xmb_icon_prefix)) {
        for(const auto& [suffix,name]:settings_icons) if(icon_ref.substr(xmb_icon_prefix.size())==suffix) {
            const auto path=asset_directory/"icons/theme"/(std::string(name)+".png");
            if(file_exists(path)) return path;
        }
    }

    if(icon_ref.starts_with(xmb_icon_prefix)) {
        const auto suffix = icon_ref.substr(xmb_icon_prefix.size());
        const auto filename = suffix == "psn"
            ? std::string{"xmb_icon_psn.png"}
            : std::string{"xmb_icon_"} + std::string{suffix} + ".png";
        const auto bundled = asset_directory / "icons" / filename;
        if(file_exists(bundled)) return bundled;
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
    return "custom";
}

[[nodiscard]] std::string_view current_ps3_background_value() noexcept {
    using background_type = config::config::background_type;
    switch(config::CONFIG.backgroundType) {
        case background_type::original:
            return "original";
        case background_type::wave:
            return "classic";
        case background_type::video:
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

[[nodiscard]] std::string_view locale_for_catalog_value(std::string_view value) noexcept {
    constexpr std::array<std::pair<std::string_view,std::string_view>,20> locales{{
        {"japanese","ja"},{"english.united.states","en_US"},
        {"english.united.kingdom","en_GB"},{"french","fr"},
        {"spanish","es"},{"german","de"},{"italian","it"},{"dutch","nl"},
        {"portuguese.portugal","pt_PT"},{"portuguese.brazil","pt_BR"},
        {"russian","ru"},{"korean","ko"},{"chinese.traditional","zh_TW"},
        {"chinese.simplified","zh_CN"},{"finnish","fi"},{"swedish","sv"},
        {"danish","da"},{"norwegian","no"},{"polish","pl"},{"turkish","tr"}
    }};
    for(const auto& [catalog,locale]:locales) if(value==catalog) return locale;
    return {};
}

[[nodiscard]] bool settings_dialog_top_action(
    std::string_view dialog_id, std::string_view choice_value) noexcept {
    return (dialog_id == "dialog.theme" && choice_value == "install") ||
        (dialog_id == "dialog.background" && choice_value == "brightness");
}

[[nodiscard]] std::string localized_dialog_choice_label(
    const openxmb::xmb::Catalog& catalog,
    std::string_view dialog_id,
    std::string_view choice_value) {
    const auto dialog_it = catalog.dialogs.find(dialog_id);
    if(dialog_it == catalog.dialogs.end()) {
        return {};
    }
    for(const auto& choice : dialog_it->second.choices) {
        if(settings_dialog_top_action(dialog_id, choice.value) ||
           std::string_view{choice.value} != choice_value) {
            continue;
        }
        const auto localized = catalog.text(choice.label_key);
        return localized.empty() ? choice.value : std::string{localized};
    }
    return {};
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
        std::pair{std::string_view{"5"},5},std::pair{std::string_view{"tv"},5},
        std::pair{std::string_view{"6"},6},std::pair{std::string_view{"game"},6},std::pair{std::string_view{"category.game"},6},
        std::pair{std::string_view{"7"},7},std::pair{std::string_view{"network"},7},std::pair{std::string_view{"category.network"},7},
        std::pair{std::string_view{"8"},8},std::pair{std::string_view{"psn"},8},std::pair{std::string_view{"online"},8},
        std::pair{std::string_view{"playstation.network"},8},std::pair{std::string_view{"category.online"},8},
        std::pair{std::string_view{"9"},9},std::pair{std::string_view{"friends"},9},std::pair{std::string_view{"category.friends"},9},
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
    auroreengine::gui_renderer& renderer,
    const auroreengine::texture& texture,
    const auroreengine::texture* glass_texture,
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
        glm::vec4(utils::xmb_icon_colour(openxmb::xmb::wall_clock_now()),
            static_cast<float>(alpha * xmb_web_icon_glass_alpha));
    if(config::CONFIG.iconGlassRefraction && glass_texture != nullptr && glass_texture->loaded) {
        renderer.draw_image_glass(*glass_texture, x, y, width, height, tint);
    } else {
        const auto flat_alpha =
            extent >= kActiveItemIconExtent - 0.5
                ? alpha * xmb_web_flat_active_icon_alpha
                : alpha;
        const auto flat_tint =
            glm::vec4(utils::xmb_icon_colour(openxmb::xmb::wall_clock_now()), static_cast<float>(flat_alpha));
        renderer.draw_image_a(texture, x, y, width, height, flat_tint);
    }
}

void draw_normalized_icon(
    auroreengine::gui_renderer& renderer,
    const auroreengine::texture& texture,
    const auroreengine::texture* glass_texture,
    float x,
    float y,
    double width,
    double height,
    glm::vec4 tint = glm::vec4{1.0F}
) {
    tint *= glm::vec4(utils::xmb_icon_colour(openxmb::xmb::wall_clock_now()), 1.0F);
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

void main_menu::preload(vk::Device device, vma::Allocator allocator, auroreengine::resource_loader& loader) {
    using ::menu::make_simple;
    using ::menu::make_simple_of;

    finish_media_transition();
    const bool reloading=!menus.empty();
    const auto previous_route=settings_controller?settings_controller->route().id:std::string{};
    const auto previous_selection=settings_controller?settings_controller->route().selection:0U;
    current_submenu=nullptr;submenu_stack.clear();in_submenu=false;media_depth_from=media_depth_to=0;
    settings_controller.reset();settings_catalog.reset();
    settings_previous_route.reset();
    settings_render_values.clear();settings_render_icons.clear();
    settings_icon_textures.clear();settings_value_labels.clear();
    menus.clear();

    const auto& asset_directory = config::CONFIG.asset_directory;
    menus.reserve(16);
    const auto panel_entry = [&](std::string label, std::string section, std::string icon = "icon_category_settings.png") {
        return make_simple<menu::action_menu_entry>(std::move(label), asset_directory/"icons"/icon, loader,
            [xmb = this->xmb, section = std::move(section)] { xmb->open_panel(section); return result::success; });
    };
    const auto panel_menu = [&](std::string label, std::filesystem::path icon,
                                std::vector<std::unique_ptr<menu::menu_entry>> rows) {
        return make_simple<menu::simple_menu>(std::move(label), std::move(icon), loader, std::move(rows));
    };
    {
        std::vector<std::unique_ptr<menu::menu_entry>> rows;
        rows.push_back(panel_entry("Local Profiles", "users", "icon_category_users.png"));
        rows.push_back(panel_entry("System Tools", "system", "icon_action_poweroff.png"));
        rows.push_back(panel_entry("Custom Sections", "custom", "icon_files_folder.png"));
        rows.push_back(make_simple<menu::users_menu>("Operating System Users", asset_directory/"icons/icon_category_users.png", loader, xmb, loader));
        menus.push_back(panel_menu("Users", preferred_icon(asset_directory,"xmb_icon_000.png","icon_category_users.png"),std::move(rows)));
    }
    menus.push_back(make_simple<menu::settings_menu>("Settings"_(),
        preferred_icon(asset_directory, "xmb_icon_001.png", "icon_category_settings.png"), loader, xmb, loader));
    menus.push_back(make_simple<menu::files_menu>("Photo"_(),
        preferred_icon(asset_directory, "xmb_icon_002.png", "icon_category_photo.png"), loader, xmb,
        config::CONFIG.pictures_path(), loader, menu::files_menu_profile::photo));
    menus.push_back(make_simple<menu::files_menu>("Music"_(),
        preferred_icon(asset_directory, "xmb_icon_003.png", "icon_category_music.png"), loader, xmb,
        config::CONFIG.music_path(), loader, menu::files_menu_profile::music));
    menus.push_back(make_simple<menu::files_menu>("Video"_(),
        preferred_icon(asset_directory, "xmb_icon_004.png", "icon_category_video.png"), loader, xmb,
        config::CONFIG.videos_path(), loader, menu::files_menu_profile::video));
    {
        std::vector<std::unique_ptr<menu::menu_entry>> rows;
        rows.push_back(make_simple<menu::files_menu>("Video Library",asset_directory/"icons/theme/icon_video.png",loader,xmb,config::CONFIG.videos_path(),loader,menu::files_menu_profile::video));
        rows.push_back(panel_entry("Internet Browser","browser","theme/icon_browser.png"));
        menus.push_back(panel_menu("TV / Video Services",asset_directory/"icons/theme/icon_tv.png",std::move(rows)));
    }
    {
        std::vector<std::unique_ptr<menu::menu_entry>> rows;
        rows.push_back(panel_entry("Game Library", "games", "icon_category_game.png"));
        rows.push_back(panel_entry("Emulation", "emulation", "icon_category_game.png"));
        rows.push_back(panel_entry("Disc Media", "discs", "icon_category_video.png"));
        rows.push_back(make_simple<menu::applications_menu>("Installed Applications", asset_directory/"icons/icon_category_application.png", loader, xmb,loader,::menu::categoryFilter("Game")));
        menus.push_back(panel_menu("Game",preferred_icon(asset_directory,"xmb_icon_005.png","icon_category_game.png"),std::move(rows)));
    }
    {
        std::vector<std::unique_ptr<menu::menu_entry>> rows;
        rows.push_back(panel_entry("Internet Browser", "browser", "icon_category_network.png"));
        rows.push_back(panel_entry("System Tools", "system"));
        rows.push_back(panel_entry("OpenXMB Settings", "settings"));
        menus.push_back(panel_menu("Network",preferred_icon(asset_directory,"xmb_icon_006.png","icon_category_network.png"),std::move(rows)));
    }
    {
        std::vector<std::unique_ptr<menu::menu_entry>> rows;
        rows.push_back(panel_entry("Local Account", "account", "xmb_icon_psn.png"));
        rows.push_back(panel_entry("Trophy Collection", "trophies", "theme/icon_trophy.png"));
        menus.push_back(panel_menu("PlayStation Network",preferred_icon(asset_directory,"xmb_icon_psn.png","icon_category_network.png"),std::move(rows)));
    }
    {
        std::vector<std::unique_ptr<menu::menu_entry>> rows;
        rows.push_back(panel_entry("Local Friends", "friends", "icon_category_friends.png"));
        rows.push_back(panel_entry("Local Account", "account", "xmb_icon_psn.png"));
        menus.push_back(panel_menu("Friends",preferred_icon(asset_directory,"xmb_icon_007.png","icon_category_friends.png"),std::move(rows)));
    }
    try {
        openxmb::desktop::Library library;
        for(const auto& section : library.custom_sections())
            menus.push_back(make_simple<menu::files_menu>(section.title,
                section.icon.empty() ? asset_directory/"icons/icon_files_folder.png" : section.icon,
                loader,xmb,section.folder,loader,menu::files_menu_profile::generic));
    } catch(const std::exception& error) { spdlog::warn("Could not load custom sections: {}",error.what()); }

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
        desktop_settings::populate(*settings_catalog, openxmb::localization::shell_locale::translate);
        if(auto created = openxmb::xmb::SettingsCatalogController::create(*settings_catalog); created) {
            settings_controller.emplace(std::move(*created.controller));
            std::unordered_set<std::string> loaded_icon_refs;
            for(const auto& [node_id, node] : settings_catalog->nodes) {
                (void)node_id;
                if(node.icon_ref.empty() || !loaded_icon_refs.insert(node.icon_ref).second) {
                    continue;
                }
                auto texture = std::make_unique<auroreengine::texture>(device, allocator);
                const auto path = config::CONFIG.resolve_icon(resolve_compat_icon_ref(asset_directory, node.icon_ref)
                    .value_or(asset_directory / "icons/icon_category_settings.png"));
                try {
                    loader.loadTexture(texture.get(), path);
                    std::unique_ptr<auroreengine::texture> glass_texture;
                    if(const auto normal_map = menu::xmb_web_normal_map_for_icon(path)) {
                        glass_texture = std::make_unique<auroreengine::texture>(device,allocator,vk::ImageUsageFlagBits::eSampled,vk::Format::eR8G8B8A8Unorm);
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
            settings_render_values.reserve(settings_catalog->nodes.size());
            settings_render_icons.reserve(settings_icon_textures.size());
            for(const auto& entry:settings_icon_textures)
                settings_render_icons.push_back({entry.semantic_id,entry.texture.get(),entry.glass_texture.get()});
            seed_settings_dialog_values();
            if(!reloading) apply_initial_settings_route();
            else if(!previous_route.empty()) {
                std::vector<std::size_t> path;
                if(find_settings_menu_path(*settings_catalog,"category.settings",previous_route,path)) {
                    const auto now=seconds_from_time_point(std::chrono::steady_clock::now());
                    for(const auto index:path) {
                        if(!settings_controller->set_selection(index,now)||!settings_controller->activate(now)) break;
                    }
                    static_cast<void>(settings_controller->set_selection(previous_selection,now));
                }
            }
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
            if(settings_category_active()) return result::success;
            [[fallthrough]];
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
    finish_media_transition();
    auto& menu = *menus[selected];
    auto res = current_submenu ? current_submenu->activate(action) : menu.activate(action);
    if(res == result::submenu) {
        auto& entry = current_submenu ? current_submenu->get_submenu(current_submenu->get_selected_submenu())
            : menu.get_submenu(menu.get_selected_submenu());
        if(auto submenu = dynamic_cast<menu::menu*>(&entry)) {
            const auto now=std::chrono::steady_clock::now();
            media_depth_from=in_submenu?static_cast<unsigned>(submenu_stack.size()+1):0;
            media_depth_to=media_depth_from+1;media_returning=false;
            if(current_submenu) {
                outgoing_media=capture_media_scene(*current_submenu,now,false);
                submenu_stack.push_back(current_submenu);
            }
            current_submenu=submenu;in_submenu=true;last_submenu_transition=now;
            current_submenu->on_open();
            last_selected_submenu_item=current_submenu->get_selected_submenu();
            last_selected_submenu_item_transition=now;
            return true;
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
        finish_media_transition();
        const auto now=std::chrono::steady_clock::now();
        outgoing_media=capture_media_scene(*current_submenu,now,false);
        close_after_transition=current_submenu;
        media_depth_from=static_cast<unsigned>(submenu_stack.size()+1);
        media_depth_to=media_depth_from-1;media_returning=true;
        if(submenu_stack.empty()) {current_submenu=nullptr;in_submenu=false;}
        else {current_submenu=submenu_stack.back();submenu_stack.pop_back();last_selected_submenu_item=current_submenu->get_selected_submenu();}
        last_submenu_transition=now;last_selected_submenu_item_transition=now;
        return true;
    }
    return false;
}

bool main_menu::settings_category_active() const {
    return selected == settings_category_index && settings_catalog &&
        settings_controller && !in_submenu;
}

bool main_menu::settings_chrome_collapsed() const {
    return settings_category_active() &&
        settings_controller->route().id != "category.settings";
}

float main_menu::background_blur_px() const {
    if(in_submenu || settings_chrome_collapsed()) {
        return submenu_backdrop_blur_px;
    }
    return 0.0F;
}

float main_menu::background_dim_alpha() const {
    if(in_submenu || settings_chrome_collapsed()) {
        return submenu_backdrop_dim_alpha;
    }
    return 0.0F;
}

void main_menu::seed_settings_dialog_values() {
    if(!settings_catalog) {
        return;
    }
    for(const auto& [node_id, node] : settings_catalog->nodes) {
        (void)node_id;
        if(node.action_id.starts_with("desktop.")) {
            settings_value_labels[node.id]=desktop_settings::display_value(node.action_id.substr(8),xmb);continue;
        }
        if(node.action_id=="simulate.system.language") {
            for(const auto& choice:node.choices)
                if(locale_for_catalog_value(choice.value)==config::CONFIG.language||
                   (choice.value=="english.united.states"&&config::CONFIG.language=="en"))
                    settings_value_labels[node.id]=settings_catalog->text(choice.label_key);
        }
        if(node.action_id.rfind("dialog.", 0) != 0) {
            continue;
        }
        const auto current_value = current_dialog_value(node.action_id);
        if(current_value.empty()) {
            continue;
        }
        auto label = localized_dialog_choice_label(
            *settings_catalog, node.action_id, current_value);
        if(node.action_id=="dialog.colour"&&current_value=="custom")
            label=nlohmann::json::parse(config::CONFIG.shell_snapshot()).value("theme-custom-colour",std::string{"Custom"});
        if(!label.empty()) {
            settings_value_labels[node.id] = std::move(label);
        }
        if(node.action_id=="dialog.theme") {
            if(const auto active=xmb->theme_library()->active();active&&!active->reverted&&!active->id.empty())
                settings_value_labels[node.id]=active->name;
        }
    }
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
    if(action == action::options) return true;
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

    const auto* selected_node=settings_catalog->find_node(menu_node->children[route.selection]);
    if(selected_node && selected_node->action_id.starts_with("desktop.")) {
        desktop_settings::activate(xmb,*selected_node);return true;
    }
    const auto resolved = openxmb::xmb::resolve_settings_action(
        *settings_catalog, menu_node->children[route.selection]);
    if(!resolved || !resolved.plan) {
        return false;
    }

    switch(resolved.plan->kind) {
        case openxmb::xmb::SettingsActionKind::navigate_menu: {
            const auto previous=settings_controller->route();
            const auto now=std::chrono::steady_clock::now();
            const auto step = settings_controller->activate(
                seconds_from_time_point(now));
            if(step&&step.changed) {
                settings_previous_route=previous;
                settings_route_changed=now;settings_returning=false;
            }
            return step && step.changed;
        }
        case openxmb::xmb::SettingsActionKind::open_dialog:
            if(open_settings_dialog_choice_overlay(*resolved.plan)) {
                return true;
            }
            xmb->emplace_overlay<message_overlay>("Unavailable", "This setting is not supported by this build.");
            return true;
        case openxmb::xmb::SettingsActionKind::simulated_setting:
            if(resolved.plan->may_update_settings_state) {
                if(const auto* node = settings_catalog->find_node(resolved.plan->node_id);
                   node && open_settings_choice_overlay(*node)) {
                    return true;
                }
            }
            xmb->emplace_overlay<message_overlay>("Unavailable", "This setting is not supported by this build.");
            return true;
        default:
            xmb->emplace_overlay<message_overlay>("Unavailable", "This setting is not supported by this build.");
            return true;
    }
}

bool main_menu::back_settings() {
    if(!settings_controller || settings_controller->navigation().stack.size() <= 2) {
        return false;
    }
    const auto previous=settings_controller->route();
    const auto now=std::chrono::steady_clock::now();
    const auto step = settings_controller->back(seconds_from_time_point(now));
    if(step&&step.changed) {
        settings_previous_route=previous;
        settings_route_changed=now;settings_returning=true;
    }
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

    settings_choice_panel_open = true;
    auto* overlay = xmb->emplace_overlay<app::choice_overlay>(
        labels,
        selection,
        [this, node_id = node.id, labels, choices=node.choices,
         system_language=node.action_id=="simulate.system.language"](unsigned int index) {
            settings_choice_panel_open = false;
            if(index < labels.size()) {
                settings_value_labels[node_id] = labels[index];
                config::CONFIG.preferences[node_id] = labels[index];
                if(system_language&&index<choices.size()) {
                    const auto locale=std::string(locale_for_catalog_value(choices[index].value));
                    // The language callback rebuilds the menu. Do not access
                    // this menu again after applying the real locale setting.
                    if(!locale.empty()) config::CONFIG.setLanguage(locale);
                }
                if(!config_override_read_only()) config::CONFIG.save_config();
            }
        },
        [this]() {
            settings_choice_panel_open = false;
        });
    return true;
}

bool main_menu::open_settings_dialog_choice_overlay(
    const openxmb::xmb::SettingsActionPlan& plan) {
    if(!settings_catalog) {
        return false;
    }
    if(plan.target_id=="dialog.font") {
        settings_choice_panel_open=true;auto* host=xmb;
        host->emplace_overlay<choice_overlay>(::app::detail::localize_ui_choices(std::vector<std::string>{"Original","Choose Font File..."}),0,
            [this,host](unsigned i){settings_choice_panel_open=false;if(!i){host->set_font_file("default");config::CONFIG.save_config();}
            else host->emplace_overlay<text_entry_overlay>("Font file",config::CONFIG.fontPath.string(),[host](std::string path){if(std::filesystem::is_regular_file(std::filesystem::u8path(path))){try{host->set_font_file(path);config::CONFIG.save_config();}catch(const std::exception& error){host->emplace_overlay<message_overlay>("Font",error.what());}}else host->emplace_overlay<message_overlay>("Font","Font file not found.");});},[this]{settings_choice_panel_open=false;});return true;
    }
    if(plan.target_id=="dialog.background") {
        settings_choice_panel_open=true;auto* host=xmb;
        host->emplace_overlay<choice_overlay>(::app::detail::localize_ui_choices(std::vector<std::string>{"Original","Classic","Wallpaper","Brightness"}),0,
            [this,host](unsigned i){settings_choice_panel_open=false;
                if(i<2){config::CONFIG.setBackgroundType(i?config::config::background_type::wave:config::config::background_type::original);config::CONFIG.save_config();return;}
                openxmb::xmb::CatalogNode node;node.id="category.settings.desktop.background";node.action_id=i==2?"desktop.theme.wallpaper":"desktop.display.brightness";
                if(i==3)for(const auto* value:{"0.6","0.8","1.0","1.2"})node.choices.push_back({value,value,value});
                desktop_settings::activate(host,node);},[this]{settings_choice_panel_open=false;});return true;
    }
    if(plan.target_id=="dialog.theme") {
        const auto library=xmb->theme_library();
        const auto installed=library->discover();
        const auto active=library->active();
        std::vector<std::string> labels{"Original","Classic"};
        unsigned int selection=current_ps3_theme_value()=="classic"?1U:0U;
        for(const auto& theme:installed) {
            if(active&&active->id==theme.id) selection=static_cast<unsigned int>(labels.size());
            labels.push_back(theme.name);
        }
        labels.push_back("Install / manage themes...");
        settings_choice_panel_open=true;
        auto* shell=xmb;
        shell->emplace_overlay<app::choice_overlay>(labels,selection,
            [this,shell,library,installed,labels,node_id=plan.node_id](unsigned int index) {
                settings_choice_panel_open=false;
                if(index>=labels.size()) return;
                if(index==labels.size()-1) {shell->open_panel("themes");return;}
                settings_value_labels[node_id]=labels[index];
                // Applying a pack reloads icon resources and reconstructs the
                // menu; retain no references to this instance afterwards.
                if(index>=2) {shell->apply_theme(library->apply(installed[index-2].id));return;}
                shell->apply_theme(library->revert());
                if(index==1) {
                    config::CONFIG.setBackgroundType(config::config::background_type::wave);
                    if(!config_override_read_only()) config::CONFIG.save_config();
                }
            },[this] {settings_choice_panel_open=false;});
        return true;
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
            settings_dialog_top_action(plan.target_id, choice.value);
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
    const auto original_value_label=settings_value_labels.contains(plan.node_id)?settings_value_labels.at(plan.node_id):std::string{};
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
    const auto original_colour=config::CONFIG.themeCustomColour;
    const auto original_colour_mode=config::CONFIG.themeOriginalColour;
    const auto original_background=config::CONFIG.backgroundType;
    const auto original_font=config::CONFIG.fontPath;
    const auto original_label=original_value_label;
    settings_choice_panel_open = true;
    auto* overlay = xmb->emplace_overlay<app::choice_overlay>(
        labels,
        selection,
        [this,
         dialog_id,
         node_id,
         labels,
         choices = selectable_choices](unsigned int index) {
            settings_choice_panel_open = false;
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
         original_selection,original_colour,original_colour_mode,original_background,original_font,original_label]() {
            settings_choice_panel_open = false;
            if(dialog_id=="dialog.colour") {
                config::CONFIG.setThemeCustomColour(original_colour);
                config::CONFIG.setThemeOriginalColour(original_colour_mode);
            } else if(dialog_id=="dialog.background") {
                config::CONFIG.setBackgroundType(original_background);
            } else if(dialog_id=="dialog.font") {
                config::CONFIG.setFontPath(original_font.string());
            } else if(original_selection < choices.size()) {
                (void)apply_settings_dialog_choice_value(
                    dialog_id, choices[original_selection].value, false);
            }
            settings_value_labels[node_id] = original_label;
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

void main_menu::render_settings_parent_layer(auroreengine::gui_renderer& renderer) {
    if(!settings_catalog || !settings_controller) {
        return;
    }
    const auto& stack = settings_controller->navigation().stack;
    if(stack.size() <= 2) {
        return;
    }

    const auto& parent_route = stack[stack.size() - 2];
    const auto* parent_menu = settings_catalog->find_node(parent_route.id);
    if(!parent_menu || parent_menu->children.empty() ||
       parent_route.selection >= parent_menu->children.size()) {
        return;
    }

    const auto layout = make_contained_layout(renderer);
    const auto find_icon = [this](std::string_view icon_ref)
        -> const settings_icon_texture* {
        for(const auto& icon : settings_icon_textures) {
            if(icon.semantic_id == icon_ref) {
                return &icon;
            }
        }
        return nullptr;
    };

    const auto draw_parent_icon = [&](const openxmb::xmb::CatalogNode& node,
                                      double center_x,
                                      double center_y,
                                      double alpha) {
        const auto* icon = find_icon(node.icon_ref);
        if(icon == nullptr || !icon->texture || !icon->texture->loaded) {
            return;
        }
        draw_icon(renderer, *icon->texture, icon->glass_texture.get(), layout,
            center_x, center_y, openxmb::xmb::SettingsSceneMetrics::icon_extent,
            alpha);
    };

    for(std::size_t index = 0; index < parent_menu->children.size(); ++index) {
        const auto* node = settings_catalog->find_node(parent_menu->children[index]);
        if(node == nullptr) {
            continue;
        }

        const auto selected_parent = index == parent_route.selection;
        const auto signed_delta = static_cast<int>(index) -
            static_cast<int>(parent_route.selection);
        const auto center_y = openxmb::xmb::SettingsSceneMetrics::focus_y +
            static_cast<double>(signed_delta) * settings_parent_spacing;
        if(center_y < openxmb::xmb::SettingsSceneMetrics::clip_top - 40.0 ||
           center_y > openxmb::xmb::SettingsSceneMetrics::clip_bottom + 40.0) {
            continue;
        }

        const auto distance = std::abs(signed_delta);
        auto alpha = selected_parent
            ? 0.92
            : std::max(0.12, 0.42 - static_cast<double>(distance - 1) * 0.05);
        if(center_y < 40.0) {
            alpha *= std::max(0.0, center_y / 40.0);
        }
        if(center_y > openxmb::xmb::SettingsSceneMetrics::logical_height - 20.0) {
            alpha *= std::max(0.0,
                (openxmb::xmb::SettingsSceneMetrics::logical_height - center_y) / 20.0);
        }
        draw_parent_icon(*node,
            selected_parent ? settings_parent_col_x : settings_sibling_col_x,
            center_y, alpha);
    }

    const auto arrow_alpha = 0.95F;
    const auto arrow_top = glm::vec4(206.0F / 255.0F, 206.0F / 255.0F,
        208.0F / 255.0F, arrow_alpha);
    const auto arrow_bottom = glm::vec4(120.0F / 255.0F, 120.0F / 255.0F,
        124.0F / 255.0F, arrow_alpha);
    const std::array<auroreengine::simple_renderer::vertex_data, 3> chevron{{
        {{layout.x(430.0), layout.y(openxmb::xmb::SettingsSceneMetrics::focus_y)},
            arrow_top, {0.0F, 0.5F}},
        {{layout.x(462.0), layout.y(openxmb::xmb::SettingsSceneMetrics::focus_y - 15.0)},
            arrow_top, {1.0F, 0.0F}},
        {{layout.x(462.0), layout.y(openxmb::xmb::SettingsSceneMetrics::focus_y + 15.0)},
            arrow_bottom, {1.0F, 1.0F}},
    }};
    renderer.draw_generic(chevron);
}

void main_menu::select(int index) {
    if(index == selected) {
        return;
    }
    finish_media_transition();
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

void main_menu::render(auroreengine::gui_renderer& renderer) {
    auto now = std::chrono::steady_clock::now();

    const auto submenu_progress = ease_out_cubic(
        progress(now, last_submenu_transition, transition_submenu_activate_duration));
    const auto partial = last_submenu_transition.time_since_epoch().count() == 0
        ? (in_submenu ? 1.0 : 0.0)
        : (in_submenu ? submenu_progress : 1.0 - submenu_progress);
    bool in_submenu_now = in_submenu || partial > 0.0;

    if(settings_catalog)for(const auto& [id,node]:settings_catalog->nodes)if(node.action_id.starts_with("desktop."))settings_value_labels[id]=desktop_settings::display_value(node.action_id.substr(8),xmb);
    render_crossbar(renderer, now);
    if(settings_category_active()) {
        render_settings_scene(renderer, now);
    }

    std::vector<std::pair<action, std::string>> buttons{};
    buttons.reserve(5);
    menus[selected]->get_button_actions(buttons);
    if((in_submenu_now&&current_submenu)||outgoing_media)render_submenu(renderer,now);
    if(in_submenu_now&&current_submenu)current_submenu->get_button_actions(buttons);
    if(in_submenu_now) {
        xmb->render_controller_buttons(renderer, 0.5f, 0.9f, buttons);
    }
}

void main_menu::render_settings_scene(auroreengine::gui_renderer& renderer, time_point now) {
    if(!settings_catalog || !settings_controller) {
        return;
    }

    auto& value_overrides=settings_render_values;
    value_overrides.clear();
    for(const auto& [node_id, label] : settings_value_labels) {
        value_overrides.push_back({
            .node_id = node_id,
            .localized_value = label,
        });
    }

    const auto transition_progress=progress(now,settings_route_changed,transition_submenu_activate_duration);
    const bool transitioning=settings_previous_route&&transition_progress<1.0;
    const auto presence=transitioning?openxmb::xmb::SettingsScenePresence::entering:openxmb::xmb::SettingsScenePresence::visible;
    const auto input=openxmb::xmb::make_settings_scene_input(settings_controller->route(),
        settings_controller->previous_selection(),settings_controller->focus_transition_started_seconds(),
        presence,seconds_from_time_point(settings_route_changed));
    auto sampled=openxmb::xmb::sample_settings_scene(*settings_catalog,input,
        seconds_from_time_point(now),value_overrides);
    if(!sampled) {
        return;
    }

    const openxmb::xmb::SettingsSceneRenderOptions options{
        .glass_icons=config::CONFIG.iconGlassRefraction,
        .suppress_value_labels=settings_choice_panel_open||xmb->has_choice_overlay()
    };
    if(transitioning) {
        const auto previous_input=openxmb::xmb::make_settings_scene_input(*settings_previous_route,
            settings_previous_route->selection,0.0,
            openxmb::xmb::SettingsScenePresence::exiting,seconds_from_time_point(settings_route_changed));
        auto previous=openxmb::xmb::sample_settings_scene(*settings_catalog,previous_input,
            seconds_from_time_point(now),value_overrides);
        if(previous) {
            if(!settings_returning) for(auto& row:previous.snapshot.rows) {
                row.icon_center_x-=2.0*previous.snapshot.content_x_shift;
                row.label_x-=2.0*previous.snapshot.content_x_shift;
                row.description_x-=2.0*previous.snapshot.content_x_shift;
            }
            settings_scene_renderer.render(renderer,previous.snapshot,settings_render_icons,options);
        }
        if(settings_returning) for(auto& row:sampled.snapshot.rows) {
            row.icon_center_x-=2.0*sampled.snapshot.content_x_shift;
            row.label_x-=2.0*sampled.snapshot.content_x_shift;
            row.description_x-=2.0*sampled.snapshot.content_x_shift;
        }
        renderer.push_color({1,1,1,static_cast<float>(sampled.snapshot.presence)});
        render_settings_parent_layer(renderer);
        renderer.pop_color();
    } else {
        settings_previous_route.reset();
        render_settings_parent_layer(renderer);
    }
    settings_scene_renderer.render(renderer,sampled.snapshot,settings_render_icons,options);
}

void main_menu::render_crossbar(auroreengine::gui_renderer& renderer, time_point now) {
    const auto layout = make_contained_layout(renderer);
    const xmb_canvas canvas(renderer);

    const auto category_linear = selected == last_selected
        ? 1.0
        : progress(now, last_selected_transition, transition_duration);
    const auto category_eased = ease_out_cubic(category_linear);
    const auto visual_selection = static_cast<double>(last_selected) +
        static_cast<double>(selected - last_selected) * category_eased;

    const auto submenu_linear = progress(
        now, last_submenu_transition, transition_submenu_activate_duration);
    const auto submenu_transition = last_submenu_transition.time_since_epoch().count() == 0
        ? (in_submenu ? 1.0 : 0.0)
        : media_motion::category(media_depth_from,media_depth_to,config::CONFIG.enabled("power.reduced.motion",false)?1.0:submenu_linear);
    auto chrome_transition = std::max(
        submenu_transition, settings_chrome_collapsed() ? 1.0 : 0.0);
    if(settings_category_active()&&settings_previous_route) {
        const auto p=ease_out_cubic(progress(now,settings_route_changed,transition_submenu_activate_duration));
        const double from=settings_previous_route->id!="category.settings"?1.0:0.0;
        const double to=settings_chrome_collapsed()?1.0:0.0;
        chrome_transition=from+(to-from)*p;
    }

    for(std::size_t index = 0; index < menus.size(); ++index) {
        const auto active = static_cast<int>(index) == selected;
        const auto distance = std::abs(static_cast<int>(index) - selected);
        auto center_x = kActiveCategoryX +
            (static_cast<double>(index) - visual_selection) * kCategorySpacing;
        auto center_y = kCategoryBaselineY + (active ? -10.0 : 0.0);
        auto extent = active ? kActiveCategoryIconExtent : kInactiveCategoryIconExtent;
        auto alpha = active ? 0.90 : 0.85;

        if(active) {
            center_x -= 257.0 * chrome_transition;
            extent *= 1.0 + (kInactiveItemScaleTier - 1.0) * chrome_transition;
            alpha *= 1.0 + (0.6 - 1.0) * chrome_transition;
        } else {
            alpha *= 1.0 - chrome_transition;
        }

        // The PSN sphere fills substantially more of its source canvas than
        // the numbered icon family. xmb-web measures a 0.68 correction.
        if(index == 8) {
            extent *= 0.68;
        }

        draw_icon(renderer, menus[index]->get_icon(),
            menus[index]->get_glass_icon(), layout,
            center_x, center_y, extent, alpha);

        if(active) {
            canvas.text(menus[index]->get_name(),center_x,kCategoryLabelY+18,24,
                {1,1,1,static_cast<float>(0.95*(1.0-0.55*chrome_transition))},0,true);
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
            canvas.text(entry.get_name(),kItemLabelX+x_shift,center_y+(focused?3:11),label_size,
                {text_value,text_value,text_value,static_cast<float>(alpha)},focused?xmb_canvas::pulse():0);
            if(focused&&!entry.get_description().empty()) canvas.text(entry.get_description(),kItemLabelX+x_shift,center_y+32,22,{1,1,1,static_cast<float>(alpha*0.82)});
        }
    };

    const auto rail_alpha = std::max(0.0, 1.0 - chrome_transition);
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

void main_menu::finish_media_transition() {
    outgoing_media.reset();
    if(auto* closing=std::exchange(close_after_transition,nullptr)) {
        xmb->get_window()->waitIdle();closing->on_close();
    }
}
void main_menu::show_settings() {
    finish_media_transition();
    while(in_submenu){back();finish_media_transition();}
    select(1);
}
std::unique_ptr<main_menu::media_scene_state> main_menu::capture_media_scene(menu::menu& child,time_point now,bool animate_focus) {
    auto state=std::make_unique<media_scene_state>();
    auto& presentation=state->catalog;
    const auto count=child.get_submenus_count();
    const auto selected_item=count?std::min<std::size_t>(child.get_selected_submenu(),count-1):0;
    auto previous=count&&animate_focus?std::min<std::size_t>(last_selected_submenu_item,count-1):selected_item;
    if(std::abs(static_cast<long long>(previous)-static_cast<long long>(selected_item))>10)previous=selected_item;
    const auto first=std::min(selected_item,previous)>4?std::min(selected_item,previous)-4:0;
    const auto last=count?std::min<std::size_t>(count-1,std::max(selected_item,previous)+12):0;
    openxmb::xmb::CatalogNode root;root.id="category.settings.media";root.kind=openxmb::xmb::NodeKind::menu;
    if(count)for(std::size_t i=first;i<=last;++i) {
        auto& entry=child.get_submenu(static_cast<unsigned>(i));
        openxmb::xmb::CatalogNode node;node.id="category.settings.media."+std::to_string(i);
        node.label_key=node.id+".label";node.description_key=node.id+".description";node.icon_ref=node.id+".icon";
        node.kind=dynamic_cast<const menu::menu*>(&entry)?openxmb::xmb::NodeKind::menu:openxmb::xmb::NodeKind::media;
        presentation.strings[node.label_key]=entry.get_name();presentation.strings[node.description_key]=entry.get_description();
        root.children.push_back(node.id);const auto id=node.id;presentation.nodes.emplace(id,std::move(node));
        const auto& stored=presentation.nodes.at(id);state->icons.push_back({stored.icon_ref,&entry.get_icon(),entry.get_glass_icon()});
    }
    presentation.nodes.emplace("category.settings.media",std::move(root));
    const openxmb::xmb::NavigationRoute route{openxmb::xmb::NavigationLayer::nested_menu,"category.settings.media",selected_item-first};
    const auto input=openxmb::xmb::make_settings_scene_input(route,previous-first,seconds_from_time_point(last_selected_submenu_item_transition),openxmb::xmb::SettingsScenePresence::visible);
    if(const auto sampled=openxmb::xmb::sample_settings_scene(presentation,input,seconds_from_time_point(now));sampled)state->snapshot=sampled.snapshot;
    else state->empty=!count;
    const auto* parent=submenu_stack.empty()?menus[selected].get():submenu_stack.back();
    for(unsigned i=0;i<parent->get_submenus_count();++i) {
        const auto delta=static_cast<int>(i)-static_cast<int>(parent->get_selected_submenu());
        const auto y=openxmb::xmb::SettingsSceneMetrics::focus_y+delta*settings_parent_spacing;
        if(y<-40||y>1120)continue;
        const auto& entry=parent->get_submenu(i);const auto alpha=delta==0?0.92:std::max(0.12,0.42-(std::abs(delta)-1)*0.05);
        state->parents.push_back({&entry.get_icon(),entry.get_glass_icon(),delta==0?settings_parent_col_x:settings_sibling_col_x,y,alpha});
    }
    return state;
}
void main_menu::draw_media_scene(auroreengine::gui_renderer& renderer,const media_scene_state& state,double shift,double alpha,double parent_shift) {
    const auto layout=make_contained_layout(renderer);
    for(const auto& entry:state.parents)draw_icon(renderer,*entry.icon,entry.glass,layout,entry.x+parent_shift,entry.y,openxmb::xmb::SettingsSceneMetrics::icon_extent,entry.alpha*alpha);
    auto snapshot=state.snapshot;
    for(std::size_t i=0;i<snapshot.row_count;++i) {
        auto& row=snapshot.rows[i];row.icon_center_x+=shift;row.label_x+=shift;row.description_x+=shift;
        row.alpha*=alpha;row.value_alpha*=alpha;row.description_alpha*=alpha;
    }
    settings_scene_renderer.render(renderer,snapshot,state.icons,{.glass_icons=config::CONFIG.iconGlassRefraction});
    if(state.empty)xmb_canvas(renderer).text("This folder is empty.",683+static_cast<float>(shift),515,34,{1,1,1,static_cast<float>(alpha)});
}
void main_menu::render_submenu(auroreengine::gui_renderer& renderer,time_point now) {
    const auto raw=config::CONFIG.enabled("power.reduced.motion",false)?1.0:progress(now,last_submenu_transition,transition_submenu_activate_duration);
    const auto layers=media_motion::layers(media_returning,raw);
    if(outgoing_media&&raw<1)draw_media_scene(renderer,*outgoing_media,layers.outgoing_x,layers.outgoing_alpha,layers.outgoing_x);
    else finish_media_transition();
    if(in_submenu&&current_submenu) {
        auto incoming=capture_media_scene(*current_submenu,now,true);
        draw_media_scene(renderer,*incoming,layers.incoming_x,layers.incoming_alpha,0);
        if(progress(now,last_selected_submenu_item_transition,transition_submenu_item_duration)>=1)last_selected_submenu_item=current_submenu->get_selected_submenu();
    }
    const auto layout=make_contained_layout(renderer);
    const auto alpha=static_cast<float>(in_submenu?1.0:layers.outgoing_alpha);
    const std::array<auroreengine::simple_renderer::vertex_data,3> chevron{{
        {{layout.x(430),layout.y(515)},{0.81F,0.81F,0.82F,alpha},{0,0.5F}},
        {{layout.x(462),layout.y(500)},{0.81F,0.81F,0.82F,alpha},{1,0}},
        {{layout.x(462),layout.y(530)},{0.47F,0.47F,0.49F,alpha},{1,1}}}};
    renderer.draw_generic(chevron);
}

}
