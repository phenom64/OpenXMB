module;

#include <array>
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

module xmbshell.app;

import spdlog;
import glibmm;
import giomm;
import i18n;
import dreamrender;
import xmbshell.config;

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
            update_checker(app::xmbshell* xmb) : xmb(xmb) {}

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
            app::xmbshell* xmb;
    };
#endif

    std::unique_ptr<action_menu_entry> entry_base(dreamrender::resource_loader& loader,
        std::string name, std::string description, const std::string& key,
        std::function<result()> callback)
    {
        std::string filename = std::format("icon_settings_{}.png", key);
        return make_simple<action_menu_entry>(std::move(name), config::CONFIG.asset_directory/"icons"/filename, loader, callback, std::function<result(action)>{}, std::move(description));
    }

    std::unique_ptr<action_menu_entry> entry_bool(dreamrender::resource_loader& loader, app::xmbshell* xmb,
        std::string name, std::string description, const std::string& schema, const std::string& key)
    {
        return entry_base(loader, std::move(name), std::move(description), key, [xmb, key, schema](){
            auto settings = Gio::Settings::create(schema);
            bool value = settings->get_boolean(key);
            xmb->emplace_overlay<app::choice_overlay>(
                std::vector<std::string>{"Off"_(), "On"_()}, value ? 1u : 0u,
                [settings, schema, key](unsigned int choice) {
                    settings->set_boolean(key, choice == 1);
                    settings->apply();
                }
            );
            return result::success;
        });
    }

    std::unique_ptr<action_menu_entry> entry_int(dreamrender::resource_loader& loader, app::xmbshell* xmb,
        std::string name, std::string description, const std::string& schema, const std::string& key, int min, int max, int step = 1)
    {
        return entry_base(loader, std::move(name), std::move(description), key, [xmb, key, schema, min, max, step](){
            auto settings = Gio::Settings::create(schema);
            int value = settings->get_int(key);
            std::vector<std::string> choices;
            for(int i = min; i <= max; i += step) {
                choices.push_back(std::to_string(i));
            }
            int current_choice = static_cast<int>((value - min)/step);
            if(current_choice < 0 || current_choice >= choices.size()) {
                current_choice = 0;
            }
            xmb->emplace_overlay<app::choice_overlay>(
                choices, static_cast<unsigned int>(current_choice),
                [settings, schema, key, min, step](unsigned int choice) {
                    int value = static_cast<int>(choice)*step + min;
                    settings->set_int(key, value);
                    settings->apply();
                }
            );
            return result::success;
        });
    }
    std::unique_ptr<action_menu_entry> entry_int(dreamrender::resource_loader& loader, app::xmbshell* xmb,
        std::string name, std::string description, const std::string& schema, const std::string& key, std::ranges::range auto values)
        requires std::is_integral_v<std::ranges::range_value_t<decltype(values)>>
    {
        return entry_base(loader, std::move(name), std::move(description), key, [xmb, key, schema, values](){
            auto settings = Gio::Settings::create(schema);
            int value = settings->get_int(key);
            std::vector<std::string> choices;
            for(auto v : values) {
                choices.push_back(std::to_string(v));
            }
            unsigned int current_choice = std::find(values.begin(), values.end(), value) - values.begin();
            xmb->emplace_overlay<app::choice_overlay>(
                choices, current_choice,
                [settings, schema, key, values](unsigned int choice) {
                    int value = *std::ranges::next(std::ranges::cbegin(values), choice);
                    settings->set_int(key, value);
                    settings->apply();
                }
            );
            return result::success;
        });
    }
    std::unique_ptr<action_menu_entry> entry_enum(dreamrender::resource_loader& loader, app::xmbshell* xmb,
        std::string name, std::string description, const std::string& schema, const std::string& key, std::ranges::range auto values)
    {
        return entry_base(loader, std::move(name), std::move(description), key, [xmb, key, schema, values](){
            auto settings = Gio::Settings::create(schema);
            std::string value = settings->get_string(key);
            std::vector<std::string> choices;
            std::vector<std::string> keys;
            for(const auto& [key, name] : values) {
                choices.push_back(name);
                keys.push_back(key);
            }
            unsigned int current_choice = std::distance(keys.begin(), std::find(keys.begin(), keys.end(), value));
            xmb->emplace_overlay<app::choice_overlay>(
                choices, current_choice,
                [settings, schema, key, keys](unsigned int choice) {
                    auto value = keys[choice];
                    settings->set_string(key, value);
                    settings->apply();
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
        constexpr char sdbus_cpp[] = {
            #embed "_deps/sdbus-cpp-src/COPYING"
        };
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

    settings_menu::settings_menu(std::string name, dreamrender::texture&& icon, app::xmbshell* xmb, dreamrender::resource_loader& loader) : simple_menu(std::move(name), std::move(icon)) {
        const std::filesystem::path& asset_dir = config::CONFIG.asset_directory;
        entries.push_back(make_simple<simple_menu>("Personalization Settings"_(), asset_dir/"icons/icon_settings_personalization.png", loader,
            std::array{
                entry_enum(loader, xmb, "Background Type"_(), "Type of background to use"_(), "re.jcm.xmbos.xmbshell", "background-type", std::array{
                    std::pair{"wave", "Animated Wave"_()},
                    std::pair{"color", "Static Color"_()},
                    std::pair{"image", "Static Image"_()},
                }),
                entry_enum(loader, xmb, "Language"_(), "Preferred language for the shell"_(), "re.jcm.xmbos.xmbshell", "language", std::array{
                    std::pair{"auto", "Use system language"_()},
                    std::pair{"en", "English"_()},
                    std::pair{"de", "German"_()},
                    std::pair{"pl", "Polish"_()},
                }),
            }
        ));
        entries.push_back(make_simple<simple_menu>("Video Settings"_(), asset_dir/"icons/icon_settings_video.png", loader,
            std::array{
                entry_bool(loader, xmb, "VSync"_(), "Avoid tearing and limit FPS to refresh rate of display"_(), "re.jcm.xmbos.xmbshell.render", "vsync"),
                entry_int(loader, xmb, "Sample Count"_(), "Number of samples used for Multisample Anti-Aliasing"_(), "re.jcm.xmbos.xmbshell.render", "sample-count", std::array{1, 2, 4, 8, 16}),
                entry_int(loader, xmb, "Max FPS"_(), "FPS limit used if VSync is disabled"_(), "re.jcm.xmbos.xmbshell.render", "max-fps", 15, 200, 5),
            }
        ));
        entries.push_back(make_simple<simple_menu>("Input Settings"_(), asset_dir/"icons/icon_settings_input.png", loader,
            std::array{
                entry_enum(loader, xmb, "Controller Type"_(), "Type of connected controller and corresponding button prompts"_(), "re.jcm.xmbos.xmbshell", "controller-type", std::array{
                    std::pair{"none", "controllertype|None"_()},
                    std::pair{"auto", "controllertype|Automatic"_()},
                    std::pair{"keyboard", "controllertype|Keyboard"_()},
                    std::pair{"playstation", "controllertype|PlayStation"_()},
                    std::pair{"xbox", "controllertype|Xbox"_()},
                    std::pair{"steam", "controllertype|Steam Controller / Steamdeck"_()},
                    std::pair{"ouya", "controllertype|Ouya"_()},
                }),
                entry_bool(loader, xmb, "Controller Rumble"_(), "Enable controller rumble as feedback for actions"_(), "re.jcm.xmbos.xmbshell", "controller-rumble"),
                entry_bool(loader, xmb, "Navigate Menus with Analog Stick"_(), "Allow navigating all menus using the analog stick in addition to the D-Pad"_(), "re.jcm.xmbos.xmbshell", "controller-analog-stick"),
            }
        ));
        entries.push_back(make_simple<simple_menu>("Debug Settings"_(), asset_dir/"icons/icon_settings_debug.png", loader,
            std::array{
                entry_bool(loader, xmb, "Show FPS"_(), "", "re.jcm.xmbos.xmbshell.render", "show-fps"),
                entry_bool(loader, xmb, "Show Memory Usage"_(), "", "re.jcm.xmbos.xmbshell.render", "show-mem"),
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
            Glib::RefPtr<Gio::Settings> shellSettings =
                Gio::Settings::create("re.jcm.xmbos.xmbshell");
            auto source = Gio::SettingsSchemaSource::get_default();
            if(!source) {
                spdlog::error("Failed to get default settings schema source");
                return result::failure;
            }
            auto schema = source->lookup("re.jcm.xmbos.xmbshell", true);
            if(!schema) {
                spdlog::error("Failed to find schema for re.jcm.xmbos.xmbshell");
                return result::failure;
            }
            for(auto key : schema->list_keys()) {
                shellSettings->reset(key);
            }
            config::CONFIG.load();
            return result::success;
        }));

        std::array licenses = {
            // NOLINTBEGIN(*-array-to-pointer-decay)
            std::make_tuple<std::string_view, std::string_view, std::string_view>("i18n-cpp", "https://github.com/JnCrMx/i18n-cpp", std::string_view(licenses::i18n_cpp, sizeof(licenses::i18n_cpp))),
#if __linux__
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
                            Gio::AppInfo::launch_default_for_uri(std::string{url});
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
