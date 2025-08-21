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