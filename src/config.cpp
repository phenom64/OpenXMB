module;

#include <glm/vec3.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <version>

module xmbshell.config;

import spdlog;
import glibmm;
import giomm;
import vulkan_hpp;
import xmbshell.constants;

namespace config {

std::optional<glm::vec3> parse_color(std::string_view hex) {
    if(hex.size() != 7 || hex[0] != '#') {
        return std::nullopt;
    }
    auto parseHex = [](std::string_view hex) -> std::optional<unsigned int> {
#if __cpp_lib_to_chars >= 201611L
        unsigned int value{};
        if(std::from_chars(hex.data(), hex.data() + hex.size(), value, 16).ec == std::errc()) {
            return value;
        }
#else
        try {
            return std::stoul(std::string(hex), nullptr, 16);
        } catch(const std::exception& e) {
            spdlog::error("Failed to parse hex value: {}", e.what());
        }
#endif
        return std::nullopt;
    };
    return glm::vec3(
        static_cast<float>(parseHex(hex.substr(1, 2)).value_or(0)) / 255.0f,
        static_cast<float>(parseHex(hex.substr(3, 2)).value_or(0)) / 255.0f,
        static_cast<float>(parseHex(hex.substr(5, 2)).value_or(0)) / 255.0f
    );
}

void config::on_update(const Glib::ustring& key) {
    reload();
    spdlog::trace("Config key changed: {}", key.c_str());
    auto cbs = callbacks.equal_range(key);
    for(auto it = cbs.first; it != cbs.second; ++it) {
        it->second(key);
    }
}

void config::load() {
    shellSettings = Gio::Settings::create("re.jcm.xmbos.xmbshell");
    shellSettings->signal_changed().connect(sigc::mem_fun(*this, &config::on_update));

    renderSettings = Gio::Settings::create("re.jcm.xmbos.xmbshell.render");
    renderSettings->signal_changed().connect(sigc::mem_fun(*this, &config::on_update));

    reload();
}

void config::reload() {
    setBackgroundColor(shellSettings->get_string("background-color"));
    auto excludedApps = shellSettings->get_string_array("excluded-applications");
    excludedApplications.clear();
    excludedApplications.insert(excludedApps.begin(), excludedApps.end());

    setWaveColor(shellSettings->get_string("wave-color"));
    setDateTimeFormat(shellSettings->get_string("date-time-format"));
    dateTimeOffset = shellSettings->get_double("date-time-x-offset");
    controllerRumble = shellSettings->get_boolean("controller-rumble");
    controllerAnalogStick = shellSettings->get_boolean("controller-analog-stick");
    controllerType = shellSettings->get_string("controller-type");

    setFontPath(shellSettings->get_string("font-path"));
    setBackgroundType(shellSettings->get_string("background-type"));
    backgroundImage = std::string{shellSettings->get_string("background-image")};

    setLanguage(shellSettings->get_string("language"));

    int sampleCount = renderSettings->get_int("sample-count");
    switch(sampleCount) {
        case 1: setSampleCount(vk::SampleCountFlagBits::e1); break;
        case 2: setSampleCount(vk::SampleCountFlagBits::e2); break;
        case 4: setSampleCount(vk::SampleCountFlagBits::e4); break;
        case 8: setSampleCount(vk::SampleCountFlagBits::e8); break;
        case 16: setSampleCount(vk::SampleCountFlagBits::e16); break;
        case 32: setSampleCount(vk::SampleCountFlagBits::e32); break;
        case 64: setSampleCount(vk::SampleCountFlagBits::e64); break;
        default: setSampleCount(vk::SampleCountFlagBits::e1); break;
    }
    setMaxFPS(renderSettings->get_int("max-fps"));
    bool vsync = renderSettings->get_boolean("vsync");
    if(vsync) {
        preferredPresentMode = vk::PresentModeKHR::eFifoRelaxed;
    } else {
        preferredPresentMode = vk::PresentModeKHR::eMailbox;
    }

    showFPS = renderSettings->get_boolean("show-fps");
    showMemory = renderSettings->get_boolean("show-mem");
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
    if(std::filesystem::exists(path)) {
        fontPath = path;
    } else {
        spdlog::warn("Ignoring invalid font path: {}", path);
        fontPath = fallback_font;
    }
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
    auto color = parse_color(hex);
    if(color) {
        backgroundColor = *color;
    } else {
        spdlog::error("Ignoring invalid hex background-color: {}", hex);
    }
}
void config::setBackgroundColor(const std::string& hex) {
    setBackgroundColor(std::string_view(hex));
}

void config::setWaveColor(glm::vec3 color) {
    waveColor = color;
}
void config::setWaveColor(std::string_view hex) {
    auto color = parse_color(hex);
    if(color) {
        waveColor = *color;
    } else {
        spdlog::error("Ignoring invalid hex wave-color: {}", hex);
    }
}
void config::setWaveColor(const std::string& hex) {
    setWaveColor(std::string_view(hex));
}

void config::setDateTimeFormat(const std::string& format) {
#if __cpp_lib_chrono >= 201907L || defined(__GLIBCXX__)
    auto now = std::chrono::zoned_time(std::chrono::current_zone(), std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
#else
    auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
#endif
    try {
        std::ignore = std::vformat("{:"+format+"}", std::make_format_args(now));
        dateTimeFormat = format;
    } catch(const std::exception& e) {
        spdlog::error("Invalid date-time format: \"{}\", error is: {}", format, e.what());
        spdlog::warn("Using fallback format: {}", constants::fallback_datetime_format);
        dateTimeFormat = constants::fallback_datetime_format;
    }
}

void config::setLanguage(const std::string& lang) {
    language = lang;
#if __linux__
    if(!language.empty() && language != "auto") {
        setenv("LANGUAGE", language.c_str(), 1);
    } else {
        unsetenv("LANGUAGE");
    }
#else
    Glib::setenv("LANGUAGE", language, true);
#endif
}

void config::excludeApplication(const std::string& application, bool exclude) {
    if(exclude) {
        excludedApplications.emplace(application);
    } else {
        excludedApplications.erase(application);
    }
    shellSettings->set_string_array("excluded-applications",
        std::vector<Glib::ustring>(excludedApplications.begin(), excludedApplications.end()));
    shellSettings->apply();
}

}
