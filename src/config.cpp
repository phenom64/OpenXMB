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

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <format>
#include <system_error>
#include <glm/vec3.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <cstdlib>

#if _WIN32
#include <shlobj.h>
#endif

module openxmb.config;

import openxmb.constants;

namespace config
{

namespace
{
    constexpr int current_config_version = 3;
    constexpr double min_controller_cursor_speed = 0.10;
    constexpr double max_controller_cursor_speed = 4.0;

    bool replace_config_file(const std::filesystem::path& temporary,
                             const std::filesystem::path& destination,
                             std::error_code& error) noexcept
    {
#if defined(_WIN32)
        if(MoveFileExW(temporary.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
            error.clear();
            return true;
        }
        error = std::error_code(static_cast<int>(GetLastError()),
                                std::system_category());
        return false;
#else
        std::filesystem::rename(temporary, destination, error);
        return !error;
#endif
    }

    const std::array<std::string, 12>& default_month_colours()
    {
        static const std::array<std::string, 12> colours = {
            "#ffff60", "#87c545", "#eb6793", "#1c5c1a",
            "#704d92", "#3ad2af", "#4aa2c2", "#c459df",
            "#ffe750", "#684d2b", "#ce4644", "#ffffff",
        };
        return colours;
    }

    const std::array<std::string, 12>& legacy_month_colours()
    {
        static const std::array<std::string, 12> colours = {
            "#f2e6a6", "#9e4540", "#4da640", "#f299cc",
            "#99cc59", "#b399e6", "#80d9f2", "#3373f2",
            "#2e2e73", "#994db3", "#cc8040", "#e64040",
        };
        return colours;
    }

    const std::array<float, 24>& default_hour_brightness()
    {
        static const std::array<float, 24> brightness = {
            0.05f, 0.05f, 0.05f, 0.05f,
            0.10f, 0.15f, 0.25f, 0.35f,
            0.45f, 0.60f, 0.75f, 0.90f,
            1.00f, 0.95f, 0.85f, 0.75f,
            0.60f, 0.50f, 0.40f, 0.30f,
            0.20f, 0.12f, 0.08f, 0.06f,
        };
        return brightness;
    }

    bool colour_equal(glm::vec3 a, glm::vec3 b)
    {
        return a.r == b.r && a.g == b.g && a.b == b.b;
    }

    int colour_channel(float channel)
    {
        return std::clamp(static_cast<int>(std::clamp(channel, 0.0f, 1.0f) * 255.0f + 0.5f), 0, 255);
    }

    std::string colour_to_hex(glm::vec3 colour)
    {
        return "#" + std::format("{:02x}{:02x}{:02x}",
            colour_channel(colour.r),
            colour_channel(colour.g),
            colour_channel(colour.b));
    }

    std::optional<glm::vec3> parse_hex_colour(std::string_view hex)
    {
        if(!hex.empty() && hex.front() == '#') {
            hex.remove_prefix(1);
        }

        if(hex.size() != 6) {
            return std::nullopt;
        }

        for(char c : hex) {
            if(!std::isxdigit(static_cast<unsigned char>(c))) {
                return std::nullopt;
            }
        }

        try {
            std::string hex_str(hex);
            int r = std::stoi(hex_str.substr(0, 2), nullptr, 16);
            int g = std::stoi(hex_str.substr(2, 2), nullptr, 16);
            int b = std::stoi(hex_str.substr(4, 2), nullptr, 16);
            return glm::vec3(r / 255.0f, g / 255.0f, b / 255.0f);
        } catch(const std::exception&) {
            return std::nullopt;
        }
    }

    std::optional<std::string> normalise_hex_colour(std::string_view hex)
    {
        auto colour = parse_hex_colour(hex);
        if(!colour) {
            return std::nullopt;
        }
        return colour_to_hex(*colour);
    }

    std::filesystem::path expand_user_path(const std::string& path)
    {
        if(path == "~" || path.starts_with("~/")) {
            const char* home = std::getenv("HOME");
#if _WIN32
            if(home == nullptr) {
                home = std::getenv("USERPROFILE");
            }
#endif
            if(home != nullptr) {
                if(path == "~") {
                    return std::filesystem::path(home);
                }
                return std::filesystem::path(home) / path.substr(2);
            }
        }
        return path;
    }

    std::filesystem::path env_path(const char* name)
    {
        if(const char* value = std::getenv(name); value && *value) {
            return value;
        }
        return {};
    }

    bool env_flag(const char* name)
    {
        if(const char* value = std::getenv(name); value != nullptr) {
            const std::string_view text{value};
            return text == "1" || text == "true" || text == "TRUE" ||
                   text == "yes" || text == "YES";
        }
        return false;
    }

    std::filesystem::path user_config_directory()
    {
#if _WIN32
        if(auto path = env_path("LOCALAPPDATA"); !path.empty()) {
            return path / "OpenXMB";
        }
        if(auto path = env_path("APPDATA"); !path.empty()) {
            return path / "OpenXMB";
        }
#elif defined(__APPLE__)
        if(auto path = env_path("HOME"); !path.empty()) {
            return path / "Library" / "Application Support" / "OpenXMB";
        }
#else
        if(auto path = env_path("XDG_CONFIG_HOME"); !path.empty()) {
            return path / "openxmb";
        }
        if(auto path = env_path("HOME"); !path.empty()) {
            return path / ".config" / "openxmb";
        }
#endif
        return std::filesystem::current_path();
    }

    std::array<std::array<std::string, 24>, 12> build_month_time_colours(
        const std::array<std::string, 12>& month_colours)
    {
        std::array<std::array<std::string, 24>, 12> colours{};
        const auto& brightness = default_hour_brightness();

        for(size_t month = 0; month < colours.size(); ++month) {
            glm::vec3 base = parse_hex_colour(month_colours[month]).value_or(glm::vec3{1.0f});
            for(size_t hour = 0; hour < colours[month].size(); ++hour) {
                colours[month][hour] = colour_to_hex(base * brightness[hour]);
            }
        }

        return colours;
    }

    bool valid_datetime_format(const std::string& format)
    {
        if(format.empty()) {
            return false;
        }

        try {
            auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
#if __cpp_lib_chrono >= 201907L || defined(__GLIBCXX__)
            auto local_now = std::chrono::zoned_time(std::chrono::current_zone(), now);
            [[maybe_unused]] const auto formatted =
                std::vformat("{:" + format + "}",
                             std::make_format_args(local_now));
#else
            std::vformat("{:" + format + "}", std::make_format_args(now));
#endif
            return true;
        } catch(const std::exception&) {
            return false;
        }
    }

    int read_config_version(const nlohmann::json& config)
    {
        if(config.contains("_meta") && config["_meta"].is_object()
            && config["_meta"].contains("config_version") && config["_meta"]["config_version"].is_number_integer()) {
            return config["_meta"]["config_version"].get<int>();
        }
        return 0;
    }

    void ensure_object(nlohmann::json& parent, const char* key)
    {
        if(!parent.contains(key) || !parent[key].is_object()) {
            parent[key] = nlohmann::json::object();
        }
    }

    void migrate_json(nlohmann::json& config, bool& migrated)
    {
        if(!config.is_object()) {
            config = nlohmann::json::object();
            migrated = true;
        }

        int version = read_config_version(config);
        if(version > current_config_version) {
            spdlog::warn("Config version {} is newer than supported version {}; loading best effort",
                version, current_config_version);
            return;
        }

        ensure_object(config, "_meta");
        ensure_object(config, "shell");
        ensure_object(config, "controller");
        ensure_object(config, "render");

        auto& shell = config["shell"];
        auto& controller = config["controller"];
        auto& render = config["render"];

        if(shell.contains("theme-color-mode") && !shell.contains("theme-colour-mode")) {
            shell["theme-colour-mode"] = shell["theme-color-mode"];
            migrated = true;
        }
        if(shell.contains("theme-custom-color") && !shell.contains("theme-custom-colour")) {
            shell["theme-custom-colour"] = shell["theme-custom-color"];
            migrated = true;
        }
        if(shell.contains("theme-month-colors") && !shell.contains("theme-month-colours")) {
            shell["theme-month-colours"] = shell["theme-month-colors"];
            migrated = true;
        }
        if(shell.contains("theme-month-time-colors") && !shell.contains("theme-month-time-colours")) {
            shell["theme-month-time-colours"] = shell["theme-month-time-colors"];
            migrated = true;
        }
        if(controller.contains("type") && controller["type"].is_string()
            && controller["type"].get<std::string>() == "default") {
            controller["type"] = "auto";
            migrated = true;
        }
        if(controller.contains("controller-cursor-speed") && !controller.contains("cursor-speed")) {
            controller["cursor-speed"] = controller["controller-cursor-speed"];
            migrated = true;
        }

        if(!shell.contains("hide-login1-options")) {
            shell["hide-login1-options"] = false;
            migrated = true;
        }
        if(!shell.contains("hide-power-options")) {
            shell["hide-power-options"] = false;
            migrated = true;
        }
        if(!shell.contains("autostart")) {
            shell["autostart"] = false;
            migrated = true;
        }
        if(!controller.contains("cursor-speed")) {
            controller["cursor-speed"] = 1.0;
            migrated = true;
        }
        if(!shell.contains("theme-month-colours")) {
            shell["theme-month-colours"] = default_month_colours();
            migrated = true;
        }
        if(!shell.contains("theme-month-time-colours")) {
            shell["theme-month-time-colours"] = build_month_time_colours(default_month_colours());
            migrated = true;
        }

        if(version < 3) {
            if(shell.contains("theme-month-colours") &&
                shell["theme-month-colours"] == nlohmann::json(legacy_month_colours())) {
                shell["theme-month-colours"] = default_month_colours();
                shell["theme-month-time-colours"] = build_month_time_colours(default_month_colours());
                migrated = true;
            }
            if(!render.contains("icon-glass-refraction") ||
                (render["icon-glass-refraction"].is_boolean() &&
                 !render["icon-glass-refraction"].get<bool>())) {
                render["icon-glass-refraction"] = true;
                migrated = true;
            }
        }

        if(version < current_config_version) {
            config["_meta"]["config_version"] = current_config_version;
            migrated = true;
        }
    }
}

config::config()
{
    resetThemeColourStrings();
}

void set_default_user_dirs(config& cfg) {
#if _WIN32
    CHAR szPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_MYPICTURES, NULL, 0, szPath))) cfg.picturesPath = szPath;
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_MYMUSIC, NULL, 0, szPath))) cfg.musicPath = szPath;
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_MYVIDEO, NULL, 0, szPath))) cfg.videosPath = szPath;
    if(const char* profile = std::getenv("USERPROFILE"); profile != nullptr) {
        if(cfg.picturesPath.empty()) cfg.picturesPath = std::filesystem::path(profile) / "Pictures";
        if(cfg.musicPath.empty()) cfg.musicPath = std::filesystem::path(profile) / "Music";
        if(cfg.videosPath.empty()) cfg.videosPath = std::filesystem::path(profile) / "Videos";
    }
#else
    const char* home = std::getenv("HOME");
    if (home) {
        cfg.picturesPath = std::filesystem::path(home) / "Pictures";
        cfg.musicPath = std::filesystem::path(home) / "Music";
        cfg.videosPath = std::filesystem::path(home) / "Videos";
    }
#endif
}

void config::load() {
    load_from_json();
}

void config::reload() {
    load_from_json();
}

void config::save_config() {
    save_to_json();
}

std::filesystem::path config::config_path_for_read() const {
    if(auto path = env_path("OPENXMB_CONFIG"); !path.empty()) {
        return path;
    }

    auto exe_config = exe_directory / "config.json";
    if(std::filesystem::exists(exe_config)) {
        return exe_config;
    }

    return user_config_directory() / "config.json";
}

std::filesystem::path config::config_path_for_write() const {
    if(auto path = env_path("OPENXMB_CONFIG"); !path.empty()) {
        return path;
    }
    return user_config_directory() / "config.json";
}

void config::load_from_json() {
    set_default_user_dirs(*this);
    resetThemeColourStrings();
    setFontPath("default");
    std::filesystem::path config_path = config_path_for_read();
    
    if (!std::filesystem::exists(config_path)) {
        spdlog::info("Config file not found at {}, using defaults", config_path.string());
        return;
    }
    
    try {
        std::ifstream config_file(config_path);
        if (!config_file.is_open()) {
            spdlog::warn("Failed to open config file: {}", config_path.string());
            return;
        }
        
        nlohmann::json config;
        config_file >> config;
        bool migrated = false;
        migrate_json(config, migrated);
        
        // Load shell settings
        if (config.contains("shell")) {
            auto& shell = config["shell"]; 
            
            if (shell.contains("background-color")) {
                setBackgroundColor(shell["background-color"].get<std::string>());
            }
            
            if (shell.contains("wave-color")) {
                setWaveColor(shell["wave-color"].get<std::string>());
            }
            
            if (shell.contains("background-type")) {
                setBackgroundType(shell["background-type"].get<std::string>());
            }
            
            if (shell.contains("background-image")) {
                setBackgroundImage(expand_user_path(shell["background-image"].get<std::string>()).string());
            }
            
            if (shell.contains("font-path")) {
                setFontPath(expand_user_path(shell["font-path"].get<std::string>()).string());
            }
            
            if (shell.contains("date-time-format")) {
                setDateTimeFormat(shell["date-time-format"].get<std::string>());
            }
            
            if (shell.contains("date-time-x-offset")) {
                setDateTimeOffset(shell["date-time-x-offset"].get<double>());
            }
            
            if (shell.contains("language")) {
                setLanguage(shell["language"].get<std::string>());
            }

            if (shell.contains("pictures-path")) {
                picturesPath = expand_user_path(shell["pictures-path"].get<std::string>());
            }

            if (shell.contains("music-path")) {
                musicPath = expand_user_path(shell["music-path"].get<std::string>());
            }

            if (shell.contains("videos-path")) {
                videosPath = expand_user_path(shell["videos-path"].get<std::string>());
            }
            // Theme / colour scheme
            if (shell.contains("theme-colour-mode")) {
                std::string m = shell["theme-colour-mode"].get<std::string>();
                setThemeOriginalColour(m == "original");
            }
            if (shell.contains("theme-custom-colour")) {
                setThemeCustomColour(shell["theme-custom-colour"].get<std::string>());
            }
            if (shell.contains("theme-month-colours") && shell["theme-month-colours"].is_array()) {
                const auto& colours = shell["theme-month-colours"];
                for(size_t i = 0; i < themeMonthColourStrings.size() && i < colours.size(); ++i) {
                    if(colours[i].is_string()) {
                        auto normalised = normalise_hex_colour(colours[i].get<std::string>());
                        if(normalised) {
                            themeMonthColourStrings[i] = *normalised;
                        } else {
                            spdlog::warn("Ignoring invalid theme-month-colours[{}]: {}", i, colours[i].get<std::string>());
                        }
                    }
                }
                themeMonthTimeColourStrings = build_month_time_colours(themeMonthColourStrings);
            }
            if (shell.contains("theme-month-time-colours") && shell["theme-month-time-colours"].is_array()) {
                const auto& month_time_colours = shell["theme-month-time-colours"];
                for(size_t month = 0; month < themeMonthTimeColourStrings.size() && month < month_time_colours.size(); ++month) {
                    if(!month_time_colours[month].is_array()) {
                        continue;
                    }
                    const auto& hour_colours = month_time_colours[month];
                    for(size_t hour = 0; hour < themeMonthTimeColourStrings[month].size() && hour < hour_colours.size(); ++hour) {
                        if(hour_colours[hour].is_string()) {
                            auto normalised = normalise_hex_colour(hour_colours[hour].get<std::string>());
                            if(normalised) {
                                themeMonthTimeColourStrings[month][hour] = *normalised;
                            } else {
                                spdlog::warn("Ignoring invalid theme-month-time-colours[{}][{}]: {}",
                                    month, hour, hour_colours[hour].get<std::string>());
                            }
                        }
                    }
                }
            }
            if (shell.contains("hide-login1-options")) {
                setHideLogin1Options(shell["hide-login1-options"].get<bool>());
            }
            if (shell.contains("hide-power-options")) {
                setHidePowerOptions(shell["hide-power-options"].get<bool>());
            }
            if (shell.contains("autostart")) {
                setAutostart(shell["autostart"].get<bool>());
            }
            
            if (shell.contains("excluded-applications")) {
                excludedApplications.clear();
                for (const auto& app : shell["excluded-applications"]) {
                    excludedApplications.insert(app.get<std::string>());
                }
            }
        }
        
        // Load controller settings
        if (config.contains("controller")) {
            auto& controller = config["controller"];
            
            if (controller.contains("rumble")) {
                setControllerRumble(controller["rumble"].get<bool>());
            }
            
            if (controller.contains("analog-stick")) {
                setControllerAnalogStick(controller["analog-stick"].get<bool>());
            }

            if (controller.contains("cursor-speed")) {
                setControllerCursorSpeed(controller["cursor-speed"].get<double>());
            } else if (controller.contains("controller-cursor-speed")) {
                setControllerCursorSpeed(controller["controller-cursor-speed"].get<double>());
            }
            
            if (controller.contains("type")) {
                setControllerType(controller["type"].get<std::string>());
            }
        }
        
        // Load render settings
        if (config.contains("render")) {
            auto& render = config["render"];
            
            if (render.contains("sample-count")) {
                int sample_count = render["sample-count"].get<int>();
                switch(sample_count) {
                    case 1: setSampleCount(vk::SampleCountFlagBits::e1); break;
                    case 2: setSampleCount(vk::SampleCountFlagBits::e2); break;
                    case 4: setSampleCount(vk::SampleCountFlagBits::e4); break;
                    case 8: setSampleCount(vk::SampleCountFlagBits::e8); break;
                    case 16: setSampleCount(vk::SampleCountFlagBits::e16); break;
                    case 32: setSampleCount(vk::SampleCountFlagBits::e32); break;
                    case 64: setSampleCount(vk::SampleCountFlagBits::e64); break;
                    default: setSampleCount(vk::SampleCountFlagBits::e4); break;
                }
            }
            
            if (render.contains("max-fps")) {
                setMaxFPS(render["max-fps"].get<double>());
            }
            
            if (render.contains("vsync")) {
                setVSync(render["vsync"].get<bool>());
            }
            
            if (render.contains("show-fps")) {
                setShowFPS(render["show-fps"].get<bool>());
            }
            
            if (render.contains("show-mem")) {
                setShowMemory(render["show-mem"].get<bool>());
            }
            if (render.contains("icon-glass-refraction")) {
                setIconGlassRefraction(render["icon-glass-refraction"].get<bool>());
            }
        }
        
        spdlog::info("Configuration loaded successfully");
        if(migrated) {
            spdlog::info("Configuration migrated to version {}", current_config_version);
            if(env_flag("OPENXMB_CONFIG_READ_ONLY")) {
                spdlog::info(
                    "Configuration override is read-only; migration remains in memory for this run");
            } else {
                save_to_json();
            }
        }
        
    } catch (const std::exception& e) {
        spdlog::error("Error loading configuration: {}", e.what());
    }
}

void config::save_to_json() {
    std::filesystem::path config_path = config_path_for_write();
    
    try {
        if(!config_path.parent_path().empty()) {
            std::filesystem::create_directories(config_path.parent_path());
        }
        nlohmann::json config;
        config["_meta"] = {
            {"project", "OpenXMB"},
            {"description", "Configuration file for the Syndromatic Open XcrossMediaBar desktop experience."},
            {"app_version", "0.1.0-beta"},
            {"config_version", current_config_version},
            {"author", "Kavish Krishnakumar / Syndromatic Limited Bharat Britannia"},
            {"copyright", "™ & © 2025-2026. Syndromatic Ltd. All rights reserved."},
            {"license", "GPLv3"},
            {"website", "https://syndromatic.com"},
            {"generated_at", "2025-08-20T00:00:00Z"}
        };
        
        // Shell settings
        config["shell"]["background-color"] = colour_to_hex(backgroundColor);
        
        config["shell"]["wave-color"] = colour_to_hex(waveColor);
        
        switch (backgroundType) {
            case background_type::original: config["shell"]["background-type"] = "original"; break;
            case background_type::wave: config["shell"]["background-type"] = "wave"; break;
            case background_type::color: config["shell"]["background-type"] = "color"; break;
            case background_type::image: config["shell"]["background-type"] = "image"; break;
        }
        
        config["shell"]["background-image"] = backgroundImage.string();
        config["shell"]["font-path"] = fontPath.string();
        config["shell"]["date-time-format"] = dateTimeFormat;
        config["shell"]["date-time-x-offset"] = dateTimeOffset;
        config["shell"]["language"] = language;
        config["shell"]["pictures-path"] = picturesPath.string();
        config["shell"]["music-path"] = musicPath.string();
        config["shell"]["videos-path"] = videosPath.string();
        config["shell"]["hide-login1-options"] = hideLogin1Options;
        config["shell"]["hide-power-options"] = hidePowerOptions;
        config["shell"]["autostart"] = autostart;
        
        config["shell"]["excluded-applications"] = nlohmann::json::array();
        for (const auto& app : excludedApplications) {
            config["shell"]["excluded-applications"].push_back(app);
        }
        
        // Theme/colour scheme
        config["shell"]["theme-colour-mode"] = themeOriginalColour ? "original" : "custom";
        config["shell"]["theme-custom-colour"] = colour_to_hex(themeCustomColour);
        config["shell"]["theme-month-colours"] = themeMonthColourStrings;
        config["shell"]["theme-month-time-colours"] = themeMonthTimeColourStrings;

        // Controller settings
        config["controller"]["rumble"] = controllerRumble;
        config["controller"]["analog-stick"] = controllerAnalogStick;
        config["controller"]["cursor-speed"] = controllerCursorSpeed;
        config["controller"]["type"] = controllerType;
        
        // Render settings
        config["render"]["sample-count"] = static_cast<int>(sampleCount);
        config["render"]["max-fps"] = maxFPS;
        config["render"]["vsync"] = (preferredPresentMode == vk::PresentModeKHR::eFifoRelaxed);
        config["render"]["show-fps"] = showFPS;
        config["render"]["show-mem"] = showMemory;
        config["render"]["icon-glass-refraction"] = iconGlassRefraction;
        
        // Write through a temp file and rename it into place.
        std::filesystem::path temp_path = config_path;
        temp_path += ".tmp";
        std::ofstream config_file(temp_path, std::ios::trunc);
        if (config_file.is_open()) {
            config_file << config.dump(4) << '\n';
            config_file.flush();
            if(!config_file.good()) {
                spdlog::error("Failed to write temporary config file: {}", temp_path.string());
                std::error_code remove_error;
                std::filesystem::remove(temp_path, remove_error);
                return;
            }
            config_file.close();

            std::error_code rename_error;
            if(!replace_config_file(temp_path, config_path, rename_error)) {
                spdlog::error("Failed to replace config file {}: {}", config_path.string(), rename_error.message());
                std::error_code remove_error;
                std::filesystem::remove(temp_path, remove_error);
            } else {
                spdlog::info("Configuration saved successfully");
            }
        } else {
            spdlog::error("Failed to open temporary config file for writing: {}", temp_path.string());
        }
        
    } catch (const std::exception& e) {
        spdlog::error("Error saving configuration: {}", e.what());
    }
}

void config::addCallback(const std::string& key, std::function<void(const std::string&)> callback) {
    callbacks.emplace(key, callback);
}

void config::notifyCallbacks(const std::string& key, const std::string& value) {
    auto [begin, end] = callbacks.equal_range(key);
    for(auto it = begin; it != end; ++it) {
        try {
            it->second(value);
        } catch(const std::exception& e) {
            spdlog::error("Config callback for '{}' failed: {}", key, e.what());
        }
    }
}

void config::resetThemeColourStrings() {
    themeMonthColourStrings = default_month_colours();
    themeMonthTimeColourStrings = build_month_time_colours(themeMonthColourStrings);
}

void config::setSampleCount(vk::SampleCountFlagBits count) {
    if(sampleCount == count) {
        return;
    }
    sampleCount = count;
    notifyCallbacks("sample-count", std::to_string(static_cast<int>(count)));
}

void config::setMaxFPS(double fps) {
    double new_max_fps = fps;
    std::chrono::duration<double> new_frame_time;
    if(fps <= 0) {
        new_max_fps = (std::numeric_limits<double>::max)();
        new_frame_time = std::chrono::duration<double>(0);
    } else {
        new_frame_time = std::chrono::duration<double>(std::chrono::seconds(1)) / new_max_fps;
    }
    if(maxFPS == new_max_fps) {
        return;
    }
    maxFPS = new_max_fps;
    frameTime = new_frame_time;
    notifyCallbacks("max-fps", std::to_string(maxFPS));
}

void config::setFontPath(std::string path) {
    std::filesystem::path resolved;

    // If the path is explicitly valid, use it
    if(std::filesystem::exists(path)) {
        resolved = path;
    } else {
        // For "default" or any invalid value, prefer the packaged asset font
        if(path == "default" || !std::filesystem::exists(path)) {
            auto asset_default = asset_directory / "Play-Regular.ttf";
            if(std::filesystem::exists(asset_default)) {
                resolved = asset_default;
            }
        }

        if(resolved.empty() && path != "default") {
            spdlog::warn("Ignoring invalid font path: {}", path);
        }
    }

    if(resolved.empty()) {
#if defined(__APPLE__)
        // Try sensible macOS defaults
        const char* mac_candidates[] = {
            "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
            "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/Library/Fonts/Arial.ttf",
        };
        for(const char* cand : mac_candidates) {
            if(std::filesystem::exists(cand)) {
                resolved = cand;
                break;
            }
        }
#endif
    }

    if(resolved.empty()) {
        resolved = fallback_font;
    }

    if(fontPath == resolved) {
        return;
    }

    fontPath = resolved;
    notifyCallbacks("font-path", fontPath.string());
}

void config::setBackgroundType(background_type type) {
    if(backgroundType == type) {
        return;
    }
    backgroundType = type;
    std::string value = "wave";
    switch(backgroundType) {
        case background_type::original: value = "original"; break;
        case background_type::wave: value = "wave"; break;
        case background_type::color: value = "color"; break;
        case background_type::image: value = "image"; break;
    }
    notifyCallbacks("background-type", value);
}

void config::setBackgroundType(std::string_view type) {
    if(type == "original") {
        setBackgroundType(background_type::original);
    } else if(type == "wave") {
        setBackgroundType(background_type::wave);
    } else if(type == "color") {
        setBackgroundType(background_type::color);
    } else if(type == "image") {
        setBackgroundType(background_type::image);
    } else {
        spdlog::error("Ignoring invalid background-type: {}", type);
        setBackgroundType(background_type::wave);
    }
}

void config::setBackgroundType(const std::string& type) {
    setBackgroundType(std::string_view(type));
}

void config::setBackgroundImage(std::string path) {
    std::filesystem::path image_path = std::move(path);
    if(backgroundImage == image_path) {
        return;
    }
    backgroundImage = std::move(image_path);
    notifyCallbacks("background-image", backgroundImage.string());
}

void config::setBackgroundColor(glm::vec3 color) {
    if(colour_equal(backgroundColor, color)) {
        return;
    }
    backgroundColor = color;
    notifyCallbacks("background-color", colour_to_hex(backgroundColor));
}

void config::setBackgroundColor(std::string_view hex) {
    auto color = parse_hex_colour(hex);
    if(!color) {
        spdlog::error("Invalid hex color: {}", hex);
        return;
    }
    setBackgroundColor(*color);
}

void config::setBackgroundColor(const std::string& hex) {
    setBackgroundColor(std::string_view(hex));
}

void config::setWaveColor(glm::vec3 color) {
    if(colour_equal(waveColor, color)) {
        return;
    }
    waveColor = color;
    notifyCallbacks("wave-color", colour_to_hex(waveColor));
}

void config::setWaveColor(std::string_view hex) {
    auto color = parse_hex_colour(hex);
    if(!color) {
        spdlog::error("Invalid hex color: {}", hex);
        return;
    }
    setWaveColor(*color);
}

void config::setWaveColor(const std::string& hex) {
    setWaveColor(std::string_view(hex));
}

void config::setThemeOriginalColour(bool original) {
    if(themeOriginalColour == original) {
        return;
    }
    themeOriginalColour = original;
    notifyCallbacks("theme-colour-mode", themeOriginalColour ? "original" : "custom");
}

void config::setThemeCustomColour(glm::vec3 color) {
    if(colour_equal(themeCustomColour, color)) {
        return;
    }
    themeCustomColour = color;
    notifyCallbacks("theme-custom-colour", colour_to_hex(themeCustomColour));
}
void config::setThemeCustomColour(std::string_view hex) {
    auto color = parse_hex_colour(hex);
    if(!color) {
        spdlog::error("Invalid hex color: {}", hex);
        return;
    }
    setThemeCustomColour(*color);
}
void config::setThemeCustomColour(const std::string& hex) {
    setThemeCustomColour(std::string_view(hex));
}

void config::setDateTimeFormat(const std::string& format) {
    if(!valid_datetime_format(format)) {
        spdlog::warn("Ignoring invalid date-time-format: {}", format);
        if(!valid_datetime_format(dateTimeFormat)) {
            dateTimeFormat = constants::fallback_datetime_format;
            notifyCallbacks("date-time-format", dateTimeFormat);
        }
        return;
    }
    if(dateTimeFormat == format) {
        return;
    }
    dateTimeFormat = format;
    notifyCallbacks("date-time-format", dateTimeFormat);
}

void config::setDateTimeOffset(double offset) {
    if(dateTimeOffset == offset) {
        return;
    }
    dateTimeOffset = offset;
    notifyCallbacks("date-time-x-offset", std::to_string(dateTimeOffset));
}

void config::setLanguage(const std::string& lang) {
    std::string resolved = lang.empty() ? "auto" : lang;
    if(language == resolved) {
        return;
    }
    language = resolved;
    notifyCallbacks("language", language);
}

void config::setVSync(bool enabled) {
    auto desired = enabled ? vk::PresentModeKHR::eFifoRelaxed : vk::PresentModeKHR::eMailbox;
    if(preferredPresentMode == desired) {
        return;
    }
    preferredPresentMode = desired;
    notifyCallbacks("vsync", enabled ? "true" : "false");
}

void config::setShowFPS(bool show) {
    if(showFPS == show) {
        return;
    }
    showFPS = show;
    notifyCallbacks("show-fps", showFPS ? "true" : "false");
}

void config::setShowMemory(bool show) {
    if(showMemory == show) {
        return;
    }
    showMemory = show;
    notifyCallbacks("show-mem", showMemory ? "true" : "false");
}

void config::setIconGlassRefraction(bool enabled) {
    if(iconGlassRefraction == enabled) {
        return;
    }
    iconGlassRefraction = enabled;
    notifyCallbacks("icon-glass-refraction", iconGlassRefraction ? "true" : "false");
}

void config::setControllerRumble(bool enabled) {
    if(controllerRumble == enabled) {
        return;
    }
    controllerRumble = enabled;
    notifyCallbacks("controller-rumble", controllerRumble ? "true" : "false");
}

void config::setControllerAnalogStick(bool enabled) {
    if(controllerAnalogStick == enabled) {
        return;
    }
    controllerAnalogStick = enabled;
    notifyCallbacks("controller-analog-stick", controllerAnalogStick ? "true" : "false");
}

void config::setControllerCursorSpeed(double speed) {
    if(!std::isfinite(speed)) {
        spdlog::warn("Ignoring invalid controller cursor speed: {}", speed);
        return;
    }

    double clamped_speed = std::clamp(speed, min_controller_cursor_speed, max_controller_cursor_speed);
    if(clamped_speed != speed) {
        spdlog::warn("Clamped controller cursor speed from {} to {}", speed, clamped_speed);
    }
    if(controllerCursorSpeed == clamped_speed) {
        return;
    }
    controllerCursorSpeed = clamped_speed;
    notifyCallbacks("controller-cursor-speed", std::to_string(controllerCursorSpeed));
}

void config::setControllerType(const std::string& type) {
    std::string resolved = (type.empty() || type == "default") ? "auto" : type;
    if(controllerType == resolved) {
        return;
    }
    controllerType = resolved;
    notifyCallbacks("controller-type", controllerType);
}

void config::setHideLogin1Options(bool hide) {
    if(hideLogin1Options == hide) {
        return;
    }
    hideLogin1Options = hide;
    notifyCallbacks("hide-login1-options", hideLogin1Options ? "true" : "false");
}

void config::setHidePowerOptions(bool hide) {
    if(hidePowerOptions == hide) {
        return;
    }
    hidePowerOptions = hide;
    notifyCallbacks("hide-power-options", hidePowerOptions ? "true" : "false");
}

void config::setAutostart(bool enabled) {
    if(autostart == enabled) {
        return;
    }
    autostart = enabled;
    spdlog::debug("Autostart preference changed; platform service integration is not implemented yet");
    notifyCallbacks("autostart", autostart ? "true" : "false");
}

void config::excludeApplication(const std::string& application, bool exclude) {
    if(exclude) {
        excludedApplications.insert(application);
    } else {
        excludedApplications.erase(application);
    }
    notifyCallbacks("excluded-applications", application);
}

}
