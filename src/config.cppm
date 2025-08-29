module;

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <unordered_set>
#include <version>

#include <glm/vec3.hpp>

#ifdef _WIN32
#include <libloaderapi.h>
#endif

export module openxmb.config;

import vulkan_hpp;
import openxmb.constants;

export namespace config
{
    class config
    {
        public:
            config() = default;

            enum class background_type {
                original, wave, color, image
            };

#if __linux__
            std::filesystem::path exe_directory = std::filesystem::canonical("/proc/self/exe").parent_path();
#elif _WIN32
            std::filesystem::path exe_directory = [](){
                std::array<char, MAX_PATH> buffer{};
                if (GetModuleFileNameA(nullptr, buffer.data(), MAX_PATH) == 0) {
                    throw std::runtime_error("Failed to get executable path");
                }
                return std::string_view{buffer}.parent_path();
            }();
#else
            std::filesystem::path exe_directory = std::filesystem::current_path(); // best guess for other platforms
#endif
            std::filesystem::path asset_directory = [this](){
                if(auto v = std::getenv("XMB_ASSET_DIR"); v != nullptr) {
                    return std::filesystem::path(v);
                }
                return exe_directory / std::string(constants::asset_directory);
            }();
            std::filesystem::path locale_directory = [this](){
                if(auto v = std::getenv("XMB_LOCALE_DIR"); v != nullptr) {
                    return std::filesystem::path(v);
                }
                // Prefer build-tree locales (CMake symlinks) when running from build dir
                auto build_locales = exe_directory / "locales";
                if(std::filesystem::exists(build_locales)) {
                    return build_locales;
                }
                return exe_directory / std::string(constants::locale_directory);
            }();
            std::filesystem::path fallback_font = exe_directory / std::string(constants::fallback_font);

            vk::PresentModeKHR      preferredPresentMode = vk::PresentModeKHR::eFifoRelaxed; //aka VSync
            vk::SampleCountFlagBits sampleCount = vk::SampleCountFlagBits::e4; // aka Anti-aliasing

            double                          maxFPS = 60;
            std::chrono::duration<double>   frameTime = std::chrono::duration<double>(std::chrono::seconds(1))/maxFPS;

            bool showFPS    = false;
            bool showMemory = false;

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

            std::filesystem::path   picturesPath;
            std::filesystem::path   musicPath;
            std::filesystem::path   videosPath;

            std::unordered_set<std::string> excludedApplications;

            bool controllerRumble = true;
            bool controllerAnalogStick = true;

            std::string controllerType;

            void load();
            void reload();
            void save_config();
            void addCallback(const std::string& key, std::function<void(const std::string&)> callback);

            void setSampleCount(vk::SampleCountFlagBits count);
            void setMaxFPS(double fps);
            void setFontPath(std::string path);
            void setBackgroundType(background_type type);
            void setBackgroundType(std::string_view type);
            void setBackgroundType(const std::string& type);
            void setBackgroundColor(glm::vec3 color);
            void setBackgroundColor(std::string_view hex);
            void setBackgroundColor(const std::string& hex);
            void setWaveColor(glm::vec3 color);
            void setWaveColor(std::string_view hex);
            void setWaveColor(const std::string& hex);
            void setThemeCustomColour(glm::vec3 color);
            void setThemeCustomColour(std::string_view hex);
            void setThemeCustomColour(const std::string& hex);
            void setDateTimeFormat(const std::string& format);
            void setLanguage(const std::string& lang);

            void excludeApplication(const std::string& application, bool exclude = true);
        private:
            std::multimap<std::string, std::function<void(const std::string&)>> callbacks;
            void load_from_json();
            void save_to_json();
    };
    inline class config CONFIG;
}
