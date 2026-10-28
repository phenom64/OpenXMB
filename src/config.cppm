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

#include <array>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <unordered_set>
#include <version>
#include <vector>

#include <glm/vec3.hpp>
#include <fast_float/fast_float.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

export module openxmb.config;

import vulkan_hpp;
import openxmb.constants;

export namespace config
{
    class config
    {
        public:
            config();

            enum class background_type {
                original, wave, color, image, video
            };
            enum class day_night_mode {
                auto_time_of_day, day, morning, dusk, evening, night
            };

#if __linux__
            std::filesystem::path exe_directory = std::filesystem::canonical("/proc/self/exe").parent_path();
#elif _WIN32
            std::filesystem::path exe_directory = [](){
                std::array<char, 32768> buffer{};
                const auto length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
                if (length == 0 || length >= buffer.size()) {
                    return std::filesystem::current_path();
                }
                return std::filesystem::path(buffer.data()).parent_path();
            }();
#elif defined(__APPLE__)
            std::filesystem::path exe_directory = [](){
                std::uint32_t size=0;
                _NSGetExecutablePath(nullptr,&size);
                std::vector<char> buffer(size);
                if(_NSGetExecutablePath(buffer.data(),&size)!=0)
                    throw std::runtime_error("Could not locate the OpenXMB executable");
                return std::filesystem::weakly_canonical(buffer.data()).parent_path();
            }();
#else
            std::filesystem::path exe_directory = std::filesystem::current_path(); // best guess for other platforms
#endif
            std::filesystem::path asset_directory = [this](){
                if(auto v = std::getenv("XMB_ASSET_DIR"); v != nullptr) {
                    return std::filesystem::path(v);
                }
#ifdef __APPLE__
                const auto bundled=exe_directory/"../Resources/share/shell";
                if(std::filesystem::is_directory(bundled)) return bundled;
#endif
                return exe_directory / std::string(constants::asset_directory);
            }();
            std::filesystem::path locale_directory = [this](){
                if(auto v = std::getenv("XMB_LOCALE_DIR"); v != nullptr) {
                    return std::filesystem::path(v);
                }
#ifdef __APPLE__
                const auto bundled=exe_directory/"../Resources/share/locale";
                if(std::filesystem::is_directory(bundled)) return bundled;
#endif
                // Prefer build-tree locales (CMake symlinks) when running from build dir
                auto build_locales = exe_directory / "locales";
                if(std::filesystem::exists(build_locales)) {
                    return build_locales;
                }
                return exe_directory / std::string(constants::locale_directory);
            }();
            std::filesystem::path fallback_font = asset_directory / "Play-Regular.ttf";

            vk::PresentModeKHR      preferredPresentMode = vk::PresentModeKHR::eFifoRelaxed; //aka VSync
            vk::SampleCountFlagBits sampleCount = vk::SampleCountFlagBits::e4; // aka Anti-aliasing

            double                          maxFPS = 60;
            std::chrono::duration<double>   frameTime = std::chrono::duration<double>(std::chrono::seconds(1))/maxFPS;

            bool showFPS    = false;
            bool showMemory = false;
            bool iconGlassRefraction = true;
            std::string iconStyle = "default";
            double textScale = 1.0;
            glm::vec3 iconColor{1.0f};
            bool iconColorAuto=true;
            glm::vec3 textColor{1.0f};
            glm::vec3 clockColor{1.0f};
            bool wavesEnabled = true;
            bool particlesEnabled = true;
            glm::vec3 particleColor{1.0f};
            std::map<std::string, std::map<std::string, float>> waveParameters;

            std::filesystem::path   fontPath;
            background_type			backgroundType = background_type::original;
            glm::vec3               backgroundColor{};
            std::filesystem::path   backgroundImage;
            glm::vec3               waveColor{};
            std::string             dateTimeFormat = constants::fallback_datetime_format;
            double                  dateTimeOffset = 0.0;
            std::string             language;

            // Theme/colour scheme (PS3‑style)
            // When 'themeOriginalColour' is true, use dynamic month/day colour; otherwise use 'themeCustomColour'.
            bool                    themeOriginalColour = true;
            glm::vec3               themeCustomColour{0.65f, 0.30f, 0.65f};
            std::array<std::string, 12> themeMonthColourStrings{};
            std::array<std::array<std::string, 24>, 12> themeMonthTimeColourStrings{};
            day_night_mode          themeDayNightMode = day_night_mode::auto_time_of_day;

            std::filesystem::path   picturesPath;
            std::filesystem::path   musicPath;
            std::filesystem::path   videosPath;
            // Command-line home/library root; never serialised into profile settings.
            std::filesystem::path   forcedMediaDirectory;
            const std::filesystem::path& pictures_path() const {return media_path(0,picturesPath);}
            const std::filesystem::path& music_path() const {return media_path(1,musicPath);}
            const std::filesystem::path& videos_path() const {return media_path(2,videosPath);}

            std::unordered_set<std::string> excludedApplications;
            bool hideLogin1Options = false;
            bool hidePowerOptions = false;
            bool autostart = false;

            bool controllerRumble = true;
            bool controllerAnalogStick = true;
            double controllerCursorSpeed = 1.0;

            std::string controllerType;

            std::map<std::string, std::filesystem::path> themeIcons;
            std::map<std::string, std::filesystem::path> themeSounds;
            std::map<std::string, std::string> preferences;
            std::filesystem::path profileConfigPath;
            bool initialSetupRequired = true;
            std::string preference(std::string_view key, std::string fallback={}) const {
                const auto it=preferences.find(std::string(key));return it==preferences.end()?fallback:it->second;
            }
            double number(std::string_view key,double fallback) const {
                const auto value=preference(key);double result{};
                const auto [end,error]=fast_float::from_chars(value.data(),value.data()+value.size(),result);
                return error==std::errc{} && end==value.data()+value.size() && std::isfinite(result)?result:fallback;
            }
            bool enabled(std::string_view key,bool fallback=true) const {
                return preference(key,fallback?"On":"Off")=="On";
            }

            std::filesystem::path resolve_icon(const std::filesystem::path& path) const;
            std::filesystem::path resolve_sound(const std::filesystem::path& path) const;
            void reset_to_defaults();
            void switch_profile(const std::filesystem::path& path);
            std::string shell_snapshot() const;

            void load();
            void reload();
            void save_config();
            [[nodiscard]] bool save_config_checked();
            void addCallback(const std::string& key, std::function<void(const std::string&)> callback);

            void setSampleCount(vk::SampleCountFlagBits count);
            void setMaxFPS(double fps);
            void setFontPath(std::string path);
            void setBackgroundType(background_type type);
            void setBackgroundType(std::string_view type);
            void setBackgroundType(const std::string& type);
            void setBackgroundImage(std::string path);
            void setBackgroundColor(glm::vec3 color);
            void setBackgroundColor(std::string_view hex);
            void setBackgroundColor(const std::string& hex);
            void setWaveColor(glm::vec3 color);
            void setWaveColor(std::string_view hex);
            void setWaveColor(const std::string& hex);
            void setThemeOriginalColour(bool original);
            void setThemeCustomColour(glm::vec3 color);
            void setThemeCustomColour(std::string_view hex);
            void setThemeCustomColour(const std::string& hex);
            void setThemeDayNightMode(day_night_mode mode);
            void setThemeDayNightMode(std::string_view mode);
            void setThemeDayNightMode(const std::string& mode);
            void setDateTimeFormat(const std::string& format);
            void setDateTimeOffset(double offset);
            void setLanguage(const std::string& lang);
            void setVSync(bool enabled);
            void setShowFPS(bool show);
            void setShowMemory(bool show);
            void setIconGlassRefraction(bool enabled);
            void setIconStyle(std::string_view style);
            void setTextScale(double scale);
            void setIconColor(std::string_view hex);
            void setTextColor(std::string_view hex);
            void setClockColor(std::string_view hex);
            void setWavesEnabled(bool enabled);
            void setParticlesEnabled(bool enabled);
            void setParticleColor(std::string_view hex);
            void setWaveParameters(std::string_view json);
            void setControllerRumble(bool enabled);
            void setControllerAnalogStick(bool enabled);
            void setControllerCursorSpeed(double speed);
            void setControllerType(const std::string& type);
            void setHideLogin1Options(bool hide);
            void setHidePowerOptions(bool hide);
            void setAutostart(bool enabled);

            void excludeApplication(const std::string& application, bool exclude = true);
        private:
            const std::filesystem::path& media_path(std::size_t category,const std::filesystem::path& configured) const;
            mutable std::filesystem::path resolvedMediaDirectory;
            mutable std::array<std::filesystem::path,3> resolvedMediaPaths;
            std::multimap<std::string, std::function<void(const std::string&)>> callbacks;
            void notifyCallbacks(const std::string& key, const std::string& value = {});
            std::filesystem::path config_path_for_read() const;
            std::filesystem::path config_path_for_write() const;
            void resetThemeColourStrings();
            void load_from_json();
            bool save_to_json();
    };
    inline class config CONFIG;
}
