module;

#include <cstring>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
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

    // Approximate PS3 palette — monthly anchor colours (sRGB 0..1)
    static const glm::vec3 MONTH_COLOURS[12] = {
        {0.95f, 0.90f, 0.65f}, // Jan  – pale yellow
        {0.62f, 0.27f, 0.25f}, // Feb  – red/brown
        {0.30f, 0.65f, 0.25f}, // Mar  – green
        {0.95f, 0.60f, 0.80f}, // Apr  – pink
        {0.60f, 0.80f, 0.35f}, // May  – light green
        {0.70f, 0.60f, 0.90f}, // Jun  – purple
        {0.50f, 0.85f, 0.95f}, // Jul  – cyan
        {0.20f, 0.45f, 0.95f}, // Aug  – blue
        {0.18f, 0.18f, 0.45f}, // Sep  – navy
        {0.60f, 0.30f, 0.70f}, // Oct  – violet
        {0.80f, 0.50f, 0.25f}, // Nov  – orange/brown
        {0.90f, 0.25f, 0.25f}  // Dec  – red
    };

    glm::vec3 xmb_month_colour(int monthIndex)
    {
        monthIndex = (monthIndex % 12 + 12) % 12;
        return MONTH_COLOURS[monthIndex];
    }

    float xmb_hour_brightness(int hour, float minuteFrac)
    {
        static const float B[24] = {
            0.05f, 0.05f, 0.05f, 0.05f,
            0.10f, 0.15f, 0.25f, 0.35f,
            0.45f, 0.60f, 0.75f, 0.90f,
            1.00f, 0.95f, 0.85f, 0.75f,
            0.60f, 0.50f, 0.40f, 0.30f,
            0.20f, 0.12f, 0.08f, 0.06f
        };
        int h0 = (hour % 24 + 24) % 24;
        int h1 = (h0 + 1) % 24;
        return B[h0] * (1.0f - minuteFrac) + B[h1] * minuteFrac;
    }

    glm::vec3 xmb_dynamic_colour(std::chrono::system_clock::time_point now)
    {
        using namespace std::chrono;
        std::time_t t = system_clock::to_time_t(now);
        std::tm lt{};
#if defined(_WIN32)
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        int month = lt.tm_mon;         // 0..11
        int day = lt.tm_mday;          // 1..31

        // Interpolate colour within the month towards the next month by day fraction
        int daysInMonth;
        switch(month) {
            case 0: case 2: case 4: case 6: case 7: case 9: case 11: daysInMonth = 31; break;
            case 3: case 5: case 8: case 10: daysInMonth = 30; break;
            default: // Feb
                { int y = lt.tm_year + 1900; bool leap = ((y%4==0 && y%100!=0) || (y%400==0)); daysInMonth = leap ? 29 : 28; }
        }
        float frac = std::clamp((day - 1) / float(daysInMonth), 0.0f, 1.0f);
        glm::vec3 c0 = xmb_month_colour(month);
        glm::vec3 c1 = xmb_month_colour((month + 1) % 12);
        return glm::mix(c0, c1, frac);
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
