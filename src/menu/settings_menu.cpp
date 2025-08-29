module;

#include <algorithm>
#include <array>
#include <type_traits>
#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

#if __linux__
#include <sys/wait.h>
#endif

module openxmb.app;

import spdlog;
import i18n;
import dreamrender;
import openxmb.config;
import vulkan_hpp;

import :settings_menu;

import :choice_overlay;
import :message_overlay;
import :progress_overlay;

import :text_viewer;

namespace menu {
    using namespace mfk::i18n::literals;

#if __linux__
    class updater : public app::progress_item {
        public:
            updater() = default;

            status init(std::string& message) override {
                message = "Updating..."_();
                start_time = std::chrono::system_clock::now();

                int pid = fork();
                if(pid == 0) {
                    std::filesystem::path appimageupdatetool = config::CONFIG.exe_directory / "appimageupdatetool";
                    execl(appimageupdatetool.c_str(), "appimageupdatetool", "--overwrite", std::getenv("APPIMAGE"), nullptr);
                    _exit(2);
                } else {
                    this->pid = pid;
                    return status::running;
                }
            }

            status progress(double& progress, std::string& message) override {
                auto now = std::chrono::system_clock::now();
                if(now - start_time < wait_duration) {
                    // We have no idea about the actual progress, so we will just lie.
                    // I hate appimageupdatetool.
                    progress = utils::progress(now, start_time, wait_duration);
                    return status::running;
                }

                int status{};
                if(waitpid(pid, &status, WNOHANG) != 0) {
                    int exit_code = WEXITSTATUS(status);
                    if(exit_code == 0) {
                        message = "Update successful. Please restart the application to apply it."_();
                        return status::success;
                    } else {
                        message = "Failed to update."_();
                        return status::error;
                    }
                }
                return status::running;
            }

            bool cancel(std::string& message) override {
                return false;
            }
        private:
            pid_t pid{};
            std::chrono::time_point<std::chrono::system_clock> start_time;
            constexpr static auto wait_duration = std::chrono::seconds(2);
    };

    class update_checker : public app::progress_item {
        public:
            update_checker(app::shell* xmb) : xmb(xmb) {}

            status init(std::string& message) override {
                message = "Checking for updates..."_();
                start_time = std::chrono::system_clock::now();

                int pid = fork();
                if(pid == 0) {
                    std::filesystem::path appimageupdatetool = config::CONFIG.exe_directory / "appimageupdatetool";
                    execl(appimageupdatetool.c_str(), "appimageupdatetool", "--check-for-update", std::getenv("APPIMAGE"), nullptr);
                    _exit(2);
                } else {
                    this->pid = pid;
                    return status::running;
                }
            }
            status progress(double& progress, std::string& message) override {
                auto now = std::chrono::system_clock::now();
                if(now - start_time < wait_duration) {
                    progress = utils::progress(now, start_time, wait_duration);
                    return status::running;
                }

                int status{};
                if(waitpid(pid, &status, WNOHANG) != 0) {
                    int exit_code = WEXITSTATUS(status);
                    if(exit_code == 0) {
                        message = "No updates available."_();
                        return status::success;
                    } else if(exit_code == 1) { // This also gets triggered if the update tool is not found or the update check fails. Oh well...
                        message = {};
                        xmb->emplace_overlay<app::message_overlay>("Update Available"_(), "An update is available. Would you like to install it?"_(),
                            std::vector<std::string>{"Yes"_(), "No"_()}, [xmb = this->xmb](unsigned int choice){
                                if(choice == 0) {
                                    xmb->emplace_overlay<app::progress_overlay>("Updating"_(), std::make_unique<updater>());
                                }
                            }
                        );
                        return status::success;
                    } else {
                        message = "Failed to check for updates."_();
                        return status::error;
                    }
                }
                return status::running;
            }
            bool cancel(std::string& message) override {
                return false;
            }
        private:
            pid_t pid{};
            std::chrono::time_point<std::chrono::system_clock> start_time;
            constexpr static auto wait_duration = std::chrono::milliseconds(500);
            app::shell* xmb;
    };
#endif

    std::unique_ptr<action_menu_entry> entry_base(dreamrender::resource_loader& loader,
        std::string name, std::string description, const std::string& key,
        std::function<result()> callback)
    {
        std::string filename = std::format("icon_settings_{}.png", key);
        return make_simple<action_menu_entry>(std::move(name), config::CONFIG.asset_directory/"icons"/filename, loader, callback, std::function<result(action)>{}, std::move(description));
    }

    std::unique_ptr<action_menu_entry> entry_bool(dreamrender::resource_loader& loader, app::shell* xmb,
        std::string name, std::string description, const std::string& /*schema*/, const std::string& key)
    {
        return entry_base(loader, std::move(name), std::move(description), key, [xmb, key](){
            // Determine current value for this key
            unsigned int current = 0;
            if(key == "vsync") {
                current = (config::CONFIG.preferredPresentMode == vk::PresentModeKHR::eFifoRelaxed) ? 1u : 0u;
            } else if(key == "controller-rumble") {
                current = config::CONFIG.controllerRumble ? 1u : 0u;
            } else if(key == "controller-analog-stick") {
                current = config::CONFIG.controllerAnalogStick ? 1u : 0u;
            } else if(key == "show-fps") {
                current = config::CONFIG.showFPS ? 1u : 0u;
            } else if(key == "show-mem") {
                current = config::CONFIG.showMemory ? 1u : 0u;
            }

            xmb->emplace_overlay<app::choice_overlay>(
                std::vector<std::string>{"Off"_(), "On"_()}, current,
                [key](unsigned int choice) {
                    bool on = (choice == 1);
                    bool changed = false;
                    if(key == "vsync") {
                        auto desired = on ? vk::PresentModeKHR::eFifoRelaxed : vk::PresentModeKHR::eMailbox;
                        changed = (config::CONFIG.preferredPresentMode != desired);
                        config::CONFIG.preferredPresentMode = desired;
                    } else if(key == "controller-rumble") {
                        changed = (config::CONFIG.controllerRumble != on);
                        config::CONFIG.controllerRumble = on;
                    } else if(key == "controller-analog-stick") {
                        changed = (config::CONFIG.controllerAnalogStick != on);
                        config::CONFIG.controllerAnalogStick = on;
                    } else if(key == "show-fps") {
                        changed = (config::CONFIG.showFPS != on);
                        config::CONFIG.showFPS = on;
                    } else if(key == "show-mem") {
                        changed = (config::CONFIG.showMemory != on);
                        config::CONFIG.showMemory = on;
                    }
                    if(changed) {
                        config::CONFIG.save_config();
                    }
                }
            );
            return result::success;
        });
    }

    std::unique_ptr<action_menu_entry> entry_int(dreamrender::resource_loader& loader, app::shell* xmb,
        std::string name, std::string description, const std::string& /*schema*/, const std::string& key, int min, int max, int step = 1)
    {
        return entry_base(loader, std::move(name), std::move(description), key, [xmb, key, min, max, step](){
            std::vector<std::string> choices;
            for(int i = min; i <= max; i += step) {
                choices.push_back(std::to_string(i));
            }

            unsigned int current_choice = 0;
            if(key == "sample-count") {
                int sc = 4;
                switch(config::CONFIG.sampleCount) {
                    case vk::SampleCountFlagBits::e1: sc = 1; break;
                    case vk::SampleCountFlagBits::e2: sc = 2; break;
                    case vk::SampleCountFlagBits::e4: sc = 4; break;
                    case vk::SampleCountFlagBits::e8: sc = 8; break;
                    case vk::SampleCountFlagBits::e16: sc = 16; break;
                    case vk::SampleCountFlagBits::e32: sc = 32; break;
                    case vk::SampleCountFlagBits::e64: sc = 64; break;
                    default: sc = 4; break;
                }
                current_choice = static_cast<unsigned int>((sc - min) / step);
            } else if(key == "max-fps") {
                int fps = static_cast<int>(std::clamp(config::CONFIG.maxFPS, 0.0, 1000.0));
                if(fps <= 0) fps = min; // treat unlimited as min for selection
                current_choice = static_cast<unsigned int>((fps - min) / step);
            }

            xmb->emplace_overlay<app::choice_overlay>(
                choices, current_choice,
                [key, min, step](unsigned int choice) {
                    int value = static_cast<int>(choice)*step + min;
                    if(key == "sample-count") {
                        vk::SampleCountFlagBits sc = vk::SampleCountFlagBits::e4;
                        switch(value) {
                            case 1: sc = vk::SampleCountFlagBits::e1; break;
                            case 2: sc = vk::SampleCountFlagBits::e2; break;
                            case 4: sc = vk::SampleCountFlagBits::e4; break;
                            case 8: sc = vk::SampleCountFlagBits::e8; break;
                            case 16: sc = vk::SampleCountFlagBits::e16; break;
                            case 32: sc = vk::SampleCountFlagBits::e32; break;
                            case 64: sc = vk::SampleCountFlagBits::e64; break;
                            default: sc = vk::SampleCountFlagBits::e4; break;
                        }
                        if(config::CONFIG.sampleCount != sc) {
                            config::CONFIG.setSampleCount(sc);
                            config::CONFIG.save_config();
                        }
                    } else if(key == "max-fps") {
                        config::CONFIG.setMaxFPS(static_cast<double>(value));
                        config::CONFIG.save_config();
                    }
                }
            );
            return result::success;
        });
    }
    std::unique_ptr<action_menu_entry> entry_int(dreamrender::resource_loader& loader, app::shell* xmb,
        std::string name, std::string description, const std::string& /*schema*/, const std::string& key, std::ranges::range auto values)
        requires std::is_integral_v<std::ranges::range_value_t<decltype(values)>>
    {
        return entry_base(loader, std::move(name), std::move(description), key, [xmb, key, values](){
            int value = 0;
            std::vector<std::string> choices;
            for(auto v : values) {
                choices.push_back(std::to_string(v));
            }
            unsigned int current_choice = 0;
            xmb->emplace_overlay<app::choice_overlay>(
                choices, current_choice,
                [key, values](unsigned int choice) {
                    int value = *std::ranges::next(std::ranges::cbegin(values), choice);
                    if(key == "sample-count") {
                        vk::SampleCountFlagBits sc = vk::SampleCountFlagBits::e4;
                        switch(value) {
                            case 1: sc = vk::SampleCountFlagBits::e1; break;
                            case 2: sc = vk::SampleCountFlagBits::e2; break;
                            case 4: sc = vk::SampleCountFlagBits::e4; break;
                            case 8: sc = vk::SampleCountFlagBits::e8; break;
                            case 16: sc = vk::SampleCountFlagBits::e16; break;
                            case 32: sc = vk::SampleCountFlagBits::e32; break;
                            case 64: sc = vk::SampleCountFlagBits::e64; break;
                        }
                        if(config::CONFIG.sampleCount != sc) {
                            config::CONFIG.setSampleCount(sc);
                            config::CONFIG.save_config();
                        }
                    }
                }
            );
            return result::success;
        });
    }
    std::unique_ptr<action_menu_entry> entry_enum(dreamrender::resource_loader& loader, app::shell* xmb,
        std::string name, std::string description, const std::string& /*schema*/, const std::string& key, std::ranges::range auto values)
    {
        return entry_base(loader, std::move(name), std::move(description), key, [xmb, key, values](){
            std::string value = "";
            std::vector<std::string> choices;
            std::vector<std::string> keys;
            for(const auto& [key, name] : values) {
                choices.push_back(name);
                keys.push_back(key);
            }
            unsigned int current_choice = 0;
            xmb->emplace_overlay<app::choice_overlay>(
                choices, current_choice,
                [xmb, key, keys](unsigned int choice) {
                    auto value = keys[choice];
                    if(key == "background-type") {
                        config::CONFIG.setBackgroundType(value);
                        config::CONFIG.save_config();
                    } else if(key == "language") {
                        config::CONFIG.setLanguage(value);
                        config::CONFIG.save_config();
                        // Apply immediately
                        xmb->reload_language();
                    } else if(key == "controller-type") {
                        // directly assign; setLanguage has a helper, but controllerType is a plain string
                        config::CONFIG.controllerType = value;
                        config::CONFIG.save_config();
                    }
                }
            );
            return result::success;
        });
    }

    namespace licenses {
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wc23-extensions"
        // NOLINTBEGIN(*-avoid-c-arrays)
        constexpr char i18n_cpp[] = {
            #embed "_deps/i18n++-src/LICENSE.md"
        };
        constexpr char argparse[] = {
            #embed "_deps/argparse-src/LICENSE"
        };
        constexpr char vulkanmemoryallocator_hpp[] = {
            #embed "_deps/vulkanmemoryallocator-hpp-src/LICENSE"
        };
        constexpr char vulkan_hpp[] = {
            #embed "_deps/vulkan-hpp-src/LICENSE.txt"
        };
        constexpr char spdlog[] = {
            #embed "_deps/spdlog-src/LICENSE"
        };
#if __linux__
#  if __has_include("_deps/sdbus-cpp-src/COPYING")
        constexpr char sdbus_cpp[] = {
            #embed "_deps/sdbus-cpp-src/COPYING"
        };
#    define OPENXMB_HAVE_SDBUS_CPP 1
#  endif
#endif
#if _WIN32
        constexpr char glibmm[] = {
            #embed "share/licenses/glibmm-2.68/COPYING"
        };
        constexpr char sdl2[] = {
            #embed "share/licenses/SDL2/LICENSE.txt"
        };
        constexpr char freetype[] = {
            #embed "share/licenses/freetype/FTL.TXT"
        };
        constexpr char glm[] = {
            #embed "share/licenses/glm/copying.txt"
        };
#else
        constexpr char glibmm[] = {
            "LGPL-2.1"
        };
        constexpr char sdl2[] = {
            "Zlib License"
        };
        constexpr char freetype[] = {
            "FreeType License"
        };
        constexpr char glm[] = {
            "MIT License"
        };
#endif
        // NOLINTEND(*-avoid-c-arrays)
        #pragma clang diagnostic pop
    }

    settings_menu::settings_menu(std::string name, dreamrender::texture&& icon, app::shell* xmb, dreamrender::resource_loader& loader) : simple_menu(std::move(name), std::move(icon)) {
        const std::filesystem::path& asset_dir = config::CONFIG.asset_directory;
        entries.push_back(make_simple<simple_menu>("Personalization Settings"_(), asset_dir/"icons/icon_settings_personalization.png", loader,
            std::array{
                entry_enum(loader, xmb, "Background Type"_(), "Type of background to use"_(), "re.jcm.xmbos.shell", "background-type", std::array{
                    std::pair{"wave", "Animated Wave"_()},
                    std::pair{"color", "Static Color"_()},
                    std::pair{"image", "Static Image"_()},
                }),
                entry_enum(loader, xmb, "Language"_(), "Preferred language for the shell"_(), "re.jcm.xmbos.shell", "language", std::array{
                    std::pair{"auto", "Use system language"_()},
                    std::pair{"en", "English"_()},
                    std::pair{"de", "German"_()},
                    std::pair{"pl", "Polish"_()},
                    std::pair{"fr", "French"_()},
                    std::pair{"hi", "Hindi"_()},
                }),
            }
        ));
        entries.push_back(make_simple<simple_menu>("Video Settings"_(), asset_dir/"icons/icon_settings_video.png", loader,
            std::array{
                entry_bool(loader, xmb, "VSync"_(), "Avoid tearing and limit FPS to refresh rate of display"_(), "re.jcm.xmbos.openxmb.render", "vsync"),
                entry_int(loader, xmb, "Sample Count"_(), "Number of samples used for Multisample Anti-Aliasing"_(), "re.jcm.xmbos.openxmb.render", "sample-count", std::array{1, 2, 4, 8, 16}),
                entry_int(loader, xmb, "Max FPS"_(), "FPS limit used if VSync is disabled"_(), "re.jcm.xmbos.openxmb.render", "max-fps", 15, 200, 5),
            }
        ));
        entries.push_back(make_simple<simple_menu>("Input Settings"_(), asset_dir/"icons/icon_settings_input.png", loader,
            std::array{
                entry_enum(loader, xmb, "Controller Type"_(), "Type of connected controller and corresponding button prompts"_(), "re.jcm.xmbos.shell", "controller-type", std::array{
                    std::pair{"none", "controllertype|None"_()},
                    std::pair{"auto", "controllertype|Automatic"_()},
                    std::pair{"keyboard", "controllertype|Keyboard"_()},
                    std::pair{"playstation", "controllertype|PlayStation"_()},
                    std::pair{"xbox", "controllertype|Xbox"_()},
                    std::pair{"steam", "controllertype|Steam Controller / Steamdeck"_()},
                    std::pair{"ouya", "controllertype|Ouya"_()},
                }),
                entry_bool(loader, xmb, "Controller Rumble"_(), "Enable controller rumble as feedback for actions"_(), "re.jcm.xmbos.shell", "controller-rumble"),
                entry_bool(loader, xmb, "Navigate Menus with Analog Stick"_(), "Allow navigating all menus using the analog stick in addition to the D-Pad"_(), "re.jcm.xmbos.shell", "controller-analog-stick"),
            }
        ));
        entries.push_back(make_simple<simple_menu>("Debug Settings"_(), asset_dir/"icons/icon_settings_debug.png", loader,
            std::array{
                entry_bool(loader, xmb, "Show FPS"_(), "", "re.jcm.xmbos.openxmb.render", "show-fps"),
                entry_bool(loader, xmb, "Show Memory Usage"_(), "", "re.jcm.xmbos.openxmb.render", "show-mem"),
#ifndef NDEBUG
                make_simple<action_menu_entry>("Toggle Background Blur"_(), asset_dir/"icons/icon_settings_toggle-background-blur.png", loader, [xmb](){
                    spdlog::info("Toggling background blur");
                    xmb->set_blur_background(!xmb->get_blur_background());
                    return result::success;
                }),
                make_simple<action_menu_entry>("Toggle Ingame Mode"_(), asset_dir/"icons/icon_settings-toggle-ingame-mode.png", loader, [xmb](){
                    spdlog::info("Toggling ingame mode");
                    xmb->set_ingame_mode(!xmb->get_ingame_mode());
                    return result::success;
                }),
#endif
            }
        ));
#if __linux__
        if(std::getenv("APPIMAGE")) {
            entries.push_back(make_simple<action_menu_entry>("Check for Updates"_(), asset_dir/"icons/icon_settings_update.png", loader, [xmb](){
                spdlog::info("Update request from XMB");
                xmb->emplace_overlay<app::progress_overlay>("System Update"_(), std::make_unique<update_checker>(xmb), false);
                return result::unsupported;
            }));
        }
#endif
        entries.push_back(make_simple<action_menu_entry>("Report bug"_(), asset_dir/"icons/icon_bug.png", loader, [](){
            spdlog::info("Bug report request from XMB");
            return result::unsupported;
        }));
        entries.push_back(make_simple<action_menu_entry>("Reset all Settings to default"_(), asset_dir/"icons/icon_settings_reset.png", loader, [](){
            spdlog::info("Settings reset request from XMB");
            config::CONFIG.load();
            config::CONFIG.save_config();
            return result::success;
        }));

        std::array licenses = {
            // NOLINTBEGIN(*-array-to-pointer-decay)
            std::make_tuple<std::string_view, std::string_view, std::string_view>("i18n-cpp", "https://github.com/JnCrMx/i18n-cpp", std::string_view(licenses::i18n_cpp, sizeof(licenses::i18n_cpp))),
#if __linux__ && defined(OPENXMB_HAVE_SDBUS_CPP)
            std::make_tuple<std::string_view, std::string_view, std::string_view>("sdbus-cpp", "https://github.com/Kistler-Group/sdbus-cpp", std::string_view(licenses::sdbus_cpp, sizeof(licenses::sdbus_cpp))),
#endif
            std::make_tuple<std::string_view, std::string_view, std::string_view>("argparse", "https://github.com/p-ranav/argparse", std::string_view(licenses::argparse, sizeof(licenses::argparse))),
            std::make_tuple<std::string_view, std::string_view, std::string_view>("glibmm", "https://gitlab.gnome.org/GNOME/glibmm", std::string_view(licenses::glibmm, sizeof(licenses::glibmm))),
            std::make_tuple<std::string_view, std::string_view, std::string_view>("Vulkan-Hpp", "https://github.com/KhronosGroup/Vulkan-Hpp", std::string_view(licenses::vulkan_hpp, sizeof(licenses::vulkan_hpp))),
            std::make_tuple<std::string_view, std::string_view, std::string_view>("spdlog", "https://github.com/gabime/spdlog", std::string_view(licenses::spdlog, sizeof(licenses::spdlog))),
            std::make_tuple<std::string_view, std::string_view, std::string_view>("SDL2", "https://github.com/libsdl-org/SDL",std::string_view(licenses::sdl2, sizeof(licenses::sdl2))),
            std::make_tuple<std::string_view, std::string_view, std::string_view>("FreeType", "https://gitlab.freedesktop.org/freetype/freetype", std::string_view(licenses::freetype, sizeof(licenses::freetype))),
            std::make_tuple<std::string_view, std::string_view, std::string_view>("glm", "https://github.com/g-truc/glm", std::string_view(licenses::glm, sizeof(licenses::glm))),
            std::make_tuple<std::string_view, std::string_view, std::string_view>("VulkanMemoryAllocator-Hpp", "https://github.com/YaaZ/VulkanMemoryAllocator-Hpp", std::string_view(licenses::vulkanmemoryallocator_hpp, sizeof(licenses::vulkanmemoryallocator_hpp))),
            // NOLINTEND(*-array-to-pointer-decay)
        };
        std::vector<std::unique_ptr<menu_entry>> license_entries;
        for(const auto& [name, url, license_text] : licenses) {
            license_entries.push_back(make_simple<action_menu_entry>(std::string{name}, asset_dir/"icons/icon_license.png", loader, [xmb, name, url, license_text](){
                xmb->emplace_overlay<app::choice_overlay>(
                    std::vector<std::string>{"View License Text"_(), "Open website in browser"_()}, 0,
                    [name, url, license_text, xmb](unsigned int selection){
                        if(selection == 0) {
                            xmb->emplace_overlay<programs::text_viewer>("License for {}"_(name), license_text);
                        } else {
                            // Use system command to open URL in default browser
                            std::string command = "xdg-open " + std::string{url};
                            system(command.c_str());
                        }
                        return result::success;
                    }
                );
                return result::success;
            }));
        }
        entries.push_back(make_simple<simple_menu>("Licenses"_(), asset_dir/"icons/icon_licenses.png", loader,
            std::move(license_entries)
        ));
    }
}
