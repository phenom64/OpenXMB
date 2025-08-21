module;

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <limits>
#include <string_view>
#include <format>
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

void set_default_user_dirs(config& cfg) {
#if _WIN32
    CHAR szPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_MYPICTURES, NULL, 0, szPath))) cfg.picturesPath = szPath;
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_MYMUSIC, NULL, 0, szPath))) cfg.musicPath = szPath;
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_MYVIDEO, NULL, 0, szPath))) cfg.videosPath = szPath;
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

void config::load_from_json() {
    set_default_user_dirs(*this);
    std::filesystem::path config_path = "config.json";
    
    if (!std::filesystem::exists(config_path)) {
        spdlog::info("Config file not found, using defaults");
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
                backgroundImage = shell["background-image"].get<std::string>();
            }
            
            if (shell.contains("font-path")) {
                setFontPath(shell["font-path"].get<std::string>());
            }
            
            if (shell.contains("date-time-format")) {
                setDateTimeFormat(shell["date-time-format"].get<std::string>());
            }
            
            if (shell.contains("date-time-x-offset")) {
                dateTimeOffset = shell["date-time-x-offset"].get<double>();
            }
            
            if (shell.contains("language")) {
                setLanguage(shell["language"].get<std::string>());
            }

            if (shell.contains("pictures-path")) {
                picturesPath = shell["pictures-path"].get<std::string>();
            }

            if (shell.contains("music-path")) {
                musicPath = shell["music-path"].get<std::string>();
            }

            if (shell.contains("videos-path")) {
                videosPath = shell["videos-path"].get<std::string>();
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
                controllerRumble = controller["rumble"].get<bool>();
            }
            
            if (controller.contains("analog-stick")) {
                controllerAnalogStick = controller["analog-stick"].get<bool>();
            }
            
            if (controller.contains("type")) {
                controllerType = controller["type"].get<std::string>();
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
                bool vsync = render["vsync"].get<bool>();
                if(vsync) {
                    preferredPresentMode = vk::PresentModeKHR::eFifoRelaxed;
                } else {
                    preferredPresentMode = vk::PresentModeKHR::eMailbox;
                }
            }
            
            if (render.contains("show-fps")) {
                showFPS = render["show-fps"].get<bool>();
            }
            
            if (render.contains("show-mem")) {
                showMemory = render["show-mem"].get<bool>();
            }
        }
        
        spdlog::info("Configuration loaded successfully");
        
    } catch (const std::exception& e) {
        spdlog::error("Error loading configuration: {}", e.what());
    }
}

void config::save_to_json() {
    std::filesystem::path config_path = "config.json";
    
    try {
        nlohmann::json config;
        
        // Shell settings
        config["shell"]["background-color"] = "#" + 
            std::format("{:02x}{:02x}{:02x}", 
                static_cast<int>(backgroundColor.r * 255),
                static_cast<int>(backgroundColor.g * 255),
                static_cast<int>(backgroundColor.b * 255));
        
        config["shell"]["wave-color"] = "#" + 
            std::format("{:02x}{:02x}{:02x}", 
                static_cast<int>(waveColor.r * 255),
                static_cast<int>(waveColor.g * 255),
                static_cast<int>(waveColor.b * 255));
        
        switch (backgroundType) {
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
        
        config["shell"]["excluded-applications"] = nlohmann::json::array();
        for (const auto& app : excludedApplications) {
            config["shell"]["excluded-applications"].push_back(app);
        }
        
        // Controller settings
        config["controller"]["rumble"] = controllerRumble;
        config["controller"]["analog-stick"] = controllerAnalogStick;
        config["controller"]["type"] = controllerType;
        
        // Render settings
        config["render"]["sample-count"] = static_cast<int>(sampleCount);
        config["render"]["max-fps"] = maxFPS;
        config["render"]["vsync"] = (preferredPresentMode == vk::PresentModeKHR::eFifoRelaxed);
        config["render"]["show-fps"] = showFPS;
        config["render"]["show-mem"] = showMemory;
        
        // Write to file
        std::ofstream config_file(config_path);
        if (config_file.is_open()) {
            config_file << config.dump(4);
            spdlog::info("Configuration saved successfully");
        } else {
            spdlog::error("Failed to open config file for writing: {}", config_path.string());
        }
        
    } catch (const std::exception& e) {
        spdlog::error("Error saving configuration: {}", e.what());
    }
}

void config::addCallback(const std::string& key, std::function<void(const std::string&)> callback) {
    callbacks.emplace(key, callback);
}

void config::setSampleCount(vk::SampleCountFlagBits count) {
    sampleCount = count;
}

void config::setMaxFPS(double fps) {
    if(fps <= 0) {
        maxFPS = std::numeric_limits<double>::max();
        frameTime = std::chrono::duration<double>(0);
        return;
    }
    maxFPS = fps;
    frameTime = std::chrono::duration<double>(std::chrono::seconds(1))/maxFPS;
}

void config::setFontPath(std::string path) {
    // If the path is explicitly valid, use it
    if(std::filesystem::exists(path)) {
        fontPath = path;
        return;
    }

    // For "default" or any invalid value, prefer the packaged asset font
    if(path == "default" || !std::filesystem::exists(path)) {
        auto asset_default = asset_directory / "Play-Regular.ttf";
        if(std::filesystem::exists(asset_default)) {
            fontPath = asset_default;
            return;
        }
    }

    if(path != "default") {
        spdlog::warn("Ignoring invalid font path: {}", path);
    }

#if defined(__APPLE__)
    // Try sensible macOS defaults
    const char* mac_candidates[] = {
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf",
    };
    for(const char* cand : mac_candidates) {
        if(std::filesystem::exists(cand)) {
            fontPath = cand;
            return;
        }
    }
    // Fallback to compiled-in default relative to exe dir
    fontPath = fallback_font;
#else
    fontPath = fallback_font;
#endif
}

void config::setBackgroundType(background_type type) {
    backgroundType = type;
}

void config::setBackgroundType(std::string_view type) {
    if(type == "wave") {
        backgroundType = background_type::wave;
    } else if(type == "color") {
        backgroundType = background_type::color;
    } else if(type == "image") {
        backgroundType = background_type::image;
    } else {
        spdlog::error("Ignoring invalid background-type: {}", type);
        backgroundType = background_type::wave;
    }
}

void config::setBackgroundType(const std::string& type) {
    setBackgroundType(std::string_view(type));
}

void config::setBackgroundColor(glm::vec3 color) {
    backgroundColor = color;
}

void config::setBackgroundColor(std::string_view hex) {
    if(hex.length() < 6) {
        spdlog::error("Invalid hex color: {}", hex);
        return;
    }
    
    try {
        std::string hex_str(hex);
        if (hex_str[0] == '#') {
            hex_str = hex_str.substr(1);
        }
        
        if (hex_str.length() != 6) {
            spdlog::error("Invalid hex color length: {}", hex_str);
            return;
        }
        
        int r = std::stoi(hex_str.substr(0, 2), nullptr, 16);
        int g = std::stoi(hex_str.substr(2, 2), nullptr, 16);
        int b = std::stoi(hex_str.substr(4, 2), nullptr, 16);
        
        backgroundColor = glm::vec3(r / 255.0f, g / 255.0f, b / 255.0f);
    } catch (const std::exception& e) {
        spdlog::error("Error parsing hex color {}: {}", hex, e.what());
    }
}

void config::setBackgroundColor(const std::string& hex) {
    setBackgroundColor(std::string_view(hex));
}

void config::setWaveColor(glm::vec3 color) {
    waveColor = color;
}

void config::setWaveColor(std::string_view hex) {
    if(hex.length() < 6) {
        spdlog::error("Invalid hex color: {}", hex);
        return;
    }
    
    try {
        std::string hex_str(hex);
        if (hex_str[0] == '#') {
            hex_str = hex_str.substr(1);
        }
        
        if (hex_str.length() != 6) {
            spdlog::error("Invalid hex color length: {}", hex_str);
            return;
        }
        
        int r = std::stoi(hex_str.substr(0, 2), nullptr, 16);
        int g = std::stoi(hex_str.substr(2, 2), nullptr, 16);
        int b = std::stoi(hex_str.substr(4, 2), nullptr, 16);
        
        waveColor = glm::vec3(r / 255.0f, g / 255.0f, b / 255.0f);
    } catch (const std::exception& e) {
        spdlog::error("Error parsing hex color {}: {}", hex, e.what());
    }
}

void config::setWaveColor(const std::string& hex) {
    setWaveColor(std::string_view(hex));
}

void config::setDateTimeFormat(const std::string& format) {
    dateTimeFormat = format;
}

void config::setLanguage(const std::string& lang) {
    language = lang;
}

void config::excludeApplication(const std::string& application, bool exclude) {
    if(exclude) {
        excludedApplications.insert(application);
    } else {
        excludedApplications.erase(application);
    }
}

}
