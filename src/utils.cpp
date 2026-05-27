/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025 Syndromatic Ltd. All rights reserved
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
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <fstream>
#include <nlohmann/json.hpp>

#ifdef __GNUG__
#include <cxxabi.h>
#endif

module openxmb.utils;

import spdlog;
import glm;
import openxmb.config;

namespace utils {

    std::optional<std::filesystem::path> resolve_icon_from_json(const std::string& icon_name) {
        static std::once_flag iconInitFlag;
        static std::map<std::string, std::filesystem::path> iconCache;
        
        std::call_once(iconInitFlag, [&]() {
            // Load icon mappings from config.json or a dedicated icons.json file
            std::filesystem::path configPath = "config.json";
            if (std::filesystem::exists(configPath)) {
                try {
                    std::ifstream configFile(configPath);
                    if (configFile.is_open()) {
                        nlohmann::json config;
                        configFile >> config;
                        
                        // Check if there's an icons section in config
                        if (config.contains("icons") && config["icons"].is_object()) {
                            for (const auto& [name, path] : config["icons"].items()) {
                                iconCache[name] = std::filesystem::path(path.get<std::string>());
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("Failed to load icon configuration: {}", e.what());
                }
            }
            
            // Add some default icon paths for common system icons
            std::vector<std::filesystem::path> defaultPaths = {
                "/usr/share/icons",
                "/usr/local/share/icons",
                "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources" // macOS
            };
            
            for (const auto& basePath : defaultPaths) {
                if (std::filesystem::exists(basePath)) {
                    try {
                        for (const auto& entry : std::filesystem::directory_iterator(basePath)) {
                            if (entry.is_directory()) {
                                std::string themeName = entry.path().filename().string();
                                // Look for common icon themes
                                if (themeName == "Adwaita" || themeName == "hicolor" || 
                                    themeName == "default" || themeName == "system") {
                                    std::filesystem::path iconsPath = entry.path() / "scalable" / "apps";
                                    if (std::filesystem::exists(iconsPath)) {
                                        for (const auto& iconEntry : std::filesystem::directory_iterator(iconsPath)) {
                                            if (iconEntry.path().extension() == ".svg") {
                                                std::string iconName = iconEntry.path().stem().string();
                                                iconCache[iconName] = iconEntry.path();
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    } catch (const std::exception& e) {
                        spdlog::debug("Error scanning icon directory {}: {}", basePath.string(), e.what());
                    }
                }
            }
            
            spdlog::debug("Loaded {} icons into cache", iconCache.size());
        });
        
        auto it = iconCache.find(icon_name);
        if (it != iconCache.end()) {
            if (std::filesystem::exists(it->second)) {
                return it->second;
            }
        }
        
        // Fallback: try to find icon by name in common locations
        std::vector<std::string> extensions = {".svg", ".png", ".xpm", ".ico"};
        std::vector<std::filesystem::path> searchPaths = {
            "/usr/share/pixmaps",
            "/usr/local/share/pixmaps",
            "/usr/share/icons/hicolor/scalable/apps",
            "/usr/share/icons/Adwaita/scalable/apps"
        };
        
        for (const auto& searchPath : searchPaths) {
            if (std::filesystem::exists(searchPath)) {
                for (const auto& ext : extensions) {
                    std::filesystem::path iconPath = searchPath / (icon_name + ext);
                    if (std::filesystem::exists(iconPath)) {
                        iconCache[icon_name] = iconPath; // Cache for future use
                        return iconPath;
                    }
                }
            }
        }
        
        spdlog::warn("Icon '{}' not found", icon_name);
        return std::nullopt;
    }

    std::string to_fixed_string(double d, int n)
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(n) << d;
        return oss.str();
    }

    namespace {
        constexpr float minimum_colour_scale = 0.0001f;

        int wrap_index(int value, int size)
        {
            return (value % size + size) % size;
        }

        float max_channel(glm::vec3 colour)
        {
            return std::max({colour.r, colour.g, colour.b});
        }

        glm::vec3 clamp_colour(glm::vec3 colour)
        {
            return {
                std::clamp(colour.r, 0.0f, 1.0f),
                std::clamp(colour.g, 0.0f, 1.0f),
                std::clamp(colour.b, 0.0f, 1.0f),
            };
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

        const std::array<glm::vec3, 12>& fallback_month_colours()
        {
            static const std::array<glm::vec3, 12> colours = {
                glm::vec3{0.95f, 0.90f, 0.65f}, // Jan - pale yellow
                glm::vec3{0.62f, 0.27f, 0.25f}, // Feb - red/brown
                glm::vec3{0.30f, 0.65f, 0.25f}, // Mar - green
                glm::vec3{0.95f, 0.60f, 0.80f}, // Apr - pink
                glm::vec3{0.60f, 0.80f, 0.35f}, // May - light green
                glm::vec3{0.70f, 0.60f, 0.90f}, // Jun - purple
                glm::vec3{0.50f, 0.85f, 0.95f}, // Jul - cyan
                glm::vec3{0.20f, 0.45f, 0.95f}, // Aug - blue
                glm::vec3{0.18f, 0.18f, 0.45f}, // Sep - navy
                glm::vec3{0.60f, 0.30f, 0.70f}, // Oct - violet
                glm::vec3{0.80f, 0.50f, 0.25f}, // Nov - orange/brown
                glm::vec3{0.90f, 0.25f, 0.25f}, // Dec - red
            };
            return colours;
        }

        const std::array<float, 24>& fallback_hour_brightness()
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

        glm::vec3 fallback_month_colour(int month_index)
        {
            return fallback_month_colours()[wrap_index(month_index, 12)];
        }

        float fallback_hour_brightness_sample(int hour)
        {
            return fallback_hour_brightness()[wrap_index(hour, 24)];
        }

        glm::vec3 configured_month_colour(int month_index)
        {
            int month = wrap_index(month_index, 12);
            if(auto colour = parse_hex_colour(config::CONFIG.themeMonthColourStrings[month])) {
                return *colour;
            }
            return fallback_month_colour(month);
        }

        glm::vec3 configured_month_time_colour(int month_index, int hour)
        {
            int month = wrap_index(month_index, 12);
            int resolved_hour = wrap_index(hour, 24);
            if(auto colour = parse_hex_colour(config::CONFIG.themeMonthTimeColourStrings[month][resolved_hour])) {
                return *colour;
            }
            return configured_month_colour(month) * fallback_hour_brightness_sample(resolved_hour);
        }

        std::tm local_time(std::chrono::system_clock::time_point now)
        {
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            std::tm lt{};
#if defined(_WIN32)
            localtime_s(&lt, &t);
#else
            localtime_r(&t, &lt);
#endif
            return lt;
        }

        int days_in_month(int year, int month)
        {
            switch(wrap_index(month, 12)) {
                case 0: case 2: case 4: case 6: case 7: case 9: case 11:
                    return 31;
                case 3: case 5: case 8: case 10:
                    return 30;
                default: {
                    bool leap = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
                    return leap ? 29 : 28;
                }
            }
        }

        float ease(float x)
        {
            x = std::clamp(x, 0.0f, 1.0f);
            return x * x * (3.0f - 2.0f * x);
        }

        template<typename Sample>
        glm::vec3 resolve_month_transition(int month, int day, int days_in_resolved_month, Sample sample)
        {
            float days = static_cast<float>(std::max(days_in_resolved_month, 1));
            float r = (std::max(day, 1) - 1) / days;
            float a1 = 15.0f / days;
            float a2 = 24.0f / days;
            glm::vec3 previous = sample(month + 11);
            glm::vec3 current = sample(month);
            glm::vec3 next = sample(month + 1);

            if(r < a1) {
                return glm::mix(previous, current, ease(r / a1));
            }
            if(r < a2) {
                return current;
            }
            return glm::mix(current, next, ease((r - a2) / (1.0f - a2)));
        }

        glm::vec3 resolve_month_time_colour(const std::tm& lt, float minute_frac, int days_in_resolved_month)
        {
            int hour0 = wrap_index(lt.tm_hour, 24);
            int hour1 = wrap_index(hour0 + 1, 24);
            auto sample = [hour0, hour1, minute_frac](int month_index) {
                return glm::mix(
                    configured_month_time_colour(month_index, hour0),
                    configured_month_time_colour(month_index, hour1),
                    minute_frac);
            };
            return resolve_month_transition(lt.tm_mon, lt.tm_mday, days_in_resolved_month, sample);
        }

        float derive_brightness(glm::vec3 shaded_colour, glm::vec3 anchor_colour, float fallback_brightness)
        {
            float anchor = max_channel(anchor_colour);
            float shaded = max_channel(shaded_colour);
            if(anchor <= minimum_colour_scale) {
                return shaded > minimum_colour_scale
                    ? std::clamp(shaded, 0.0f, 1.0f)
                    : fallback_brightness;
            }
            return std::clamp(shaded / anchor, 0.0f, 1.0f);
        }

        glm::vec3 derive_base_colour(glm::vec3 shaded_colour, float brightness)
        {
            if(brightness <= minimum_colour_scale) {
                return clamp_colour(shaded_colour);
            }
            return clamp_colour(shaded_colour / brightness);
        }
    }

    glm::vec3 xmb_month_colour(int monthIndex)
    {
        return configured_month_colour(monthIndex);
    }

    float xmb_hour_brightness(int hour, float minuteFrac)
    {
        int h0 = wrap_index(hour, 24);
        int h1 = wrap_index(h0 + 1, 24);
        float t = std::clamp(minuteFrac, 0.0f, 1.0f);
        return fallback_hour_brightness_sample(h0) * (1.0f - t) + fallback_hour_brightness_sample(h1) * t;
    }

    glm::vec3 xmb_dynamic_colour(std::chrono::system_clock::time_point now)
    {
        std::tm lt = local_time(now);
        int days = days_in_month(lt.tm_year + 1900, lt.tm_mon);
        return resolve_month_transition(lt.tm_mon, lt.tm_mday, days, configured_month_colour);
    }

    xmb_resolved_theme_colour xmb_resolve_theme_colour(std::chrono::system_clock::time_point now)
    {
        std::tm lt = local_time(now);
        int days = days_in_month(lt.tm_year + 1900, lt.tm_mon);
        float minute_frac = std::clamp(static_cast<float>(lt.tm_min) / 60.0f, 0.0f, 1.0f);
        float fallback_brightness = xmb_hour_brightness(lt.tm_hour, minute_frac);

        if(!config::CONFIG.themeOriginalColour) {
            glm::vec3 base = clamp_colour(config::CONFIG.themeCustomColour);
            return {
                base,
                fallback_brightness,
                clamp_colour(base * fallback_brightness),
            };
        }

        glm::vec3 anchor = xmb_dynamic_colour(now);
        glm::vec3 shaded = clamp_colour(resolve_month_time_colour(lt, minute_frac, days));
        float brightness = derive_brightness(shaded, anchor, fallback_brightness);
        return {
            derive_base_colour(shaded, brightness),
            brightness,
            shaded,
        };
    }
}

#ifdef __GNUG__
namespace utils
{
    std::string demangle(const char *name) {
        int status = -4;
        std::unique_ptr<char, void(*)(void*)> res{
            abi::__cxa_demangle(name, nullptr, nullptr, &status),
            std::free
        };
        return (status==0) ? res.get() : name;
    }
}
#else
namespace utils
{
    std::string demangle(const char *name) {
        return std::string(name);
    }
}
#endif
