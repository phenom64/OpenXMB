module;

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <unistd.h>

module xmbshell.app;

import :users_menu;
import :menu_base;
import :menu_utils;
import :message_overlay;

import xmbshell.config;
import dreamrender;
import glibmm;
import giomm;
import sdl2;
import spdlog;
import i18n;

namespace menu {
    using namespace mfk::i18n::literals;

    users_menu::users_menu(std::string name, dreamrender::texture&& icon, app::xmbshell* xmb, dreamrender::resource_loader& loader) : simple_menu(std::move(name), std::move(icon)) {
        try {
            login1 = Gio::DBus::Proxy::create_for_bus_sync(Gio::DBus::BusType::SYSTEM, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager");
            accounts = Gio::DBus::Proxy::create_for_bus_sync(Gio::DBus::BusType::SYSTEM, "org.freedesktop.Accounts", "/org/freedesktop/Accounts", "org.freedesktop.Accounts");
        } catch (const std::exception& e) {
            spdlog::error("Failed to create DBus proxies: {}", static_cast<std::string>(e.what()));
        }

#if __linux__
        if(accounts) try {
            uint64_t my_uid = getuid();
            Glib::Variant<std::vector<Glib::DBusObjectPathString>> users;
            accounts->call_sync("ListCachedUsers", Glib::VariantContainerBase{}).get_child(users);
            for(const auto& user_path : users.get()) {
                auto user = Gio::DBus::Proxy::create_for_bus_sync(Gio::DBus::BusType::SYSTEM, "org.freedesktop.Accounts", user_path, "org.freedesktop.Accounts.User");
                Glib::Variant<Glib::ustring> real_name, icon_file;
                Glib::Variant<uint64_t> uid;
                user->get_cached_property(real_name, "RealName");
                user->get_cached_property(icon_file, "IconFile");
                user->get_cached_property(uid, "Uid");

                std::filesystem::path icon_file_path = static_cast<std::string>(icon_file.get());
                std::error_code ec;
                if(!std::filesystem::exists(icon_file_path, ec) || ec) {
                    icon_file_path = config::CONFIG.asset_directory/"icons/icon_user.png";
                }
                auto entry = make_simple<simple_menu_entry>(real_name.get(), icon_file_path, loader);

                if(uid.get() == my_uid) { // the current user is always first
                    entries.insert(entries.begin(), std::move(entry));
                } else {
                    entries.push_back(std::move(entry));
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("Failed to get user list: {}", static_cast<std::string>(e.what()));
        }
#endif

        entries.push_back(make_simple<action_menu_entry>("Quit"_(), config::CONFIG.asset_directory/"icons/icon_action_quit.png", loader, [xmb](){
            xmb->emplace_overlay<app::message_overlay>("Quit"_(), "Do you really want to quit the application?"_(),
                std::vector<std::string>{"Yes"_(), "No"_()}, [xmb](unsigned int choice){
                    if(choice == 0) {
                        sdl::Event event = {
                            .quit = {
                                .type = sdl::EventType::SDL_QUIT,
                                .timestamp = sdl::GetTicks()
                            }
                        };
                        sdl::PushEvent(&event);
                    }
                }
            );
            return result::success;
        }));

        if(login1) try {
            if(Glib::Variant<Glib::ustring> v; login1->call_sync("CanPowerOff", Glib::VariantContainerBase{}).get_child(v), v.get() == "yes") {
                entries.push_back(make_simple<action_menu_entry>("Power off"_(), config::CONFIG.asset_directory/"icons/icon_action_poweroff.png", loader, [this, xmb](){
                    xmb->emplace_overlay<app::message_overlay>("Power off"_(), "Do you really want to power off the system?"_(),
                        std::vector<std::string>{"Yes"_(), "No"_()}, [this](unsigned int choice){
                            if(choice == 0) {
                                login1->call_sync("PowerOff", Glib::VariantContainerBase::create_tuple(Glib::Variant<bool>::create(true)));
                            }
                        }
                    );
                    return result::success;
                }));
            }
            if(Glib::Variant<Glib::ustring> v; login1->call_sync("CanReboot", Glib::VariantContainerBase{}).get_child(v), v.get() == "yes") {
                entries.push_back(make_simple<action_menu_entry>("Reboot"_(), config::CONFIG.asset_directory/"icons/icon_action_reboot.png", loader, [this, xmb](){
                    xmb->emplace_overlay<app::message_overlay>("Reboot"_(), "Do you really want to reboot the system?"_(),
                        std::vector<std::string>{"Yes"_(), "No"_()}, [this](unsigned int choice){
                            if(choice == 0) {
                                login1->call_sync("Reboot", Glib::VariantContainerBase::create_tuple(Glib::Variant<bool>::create(true)));
                            }
                        }
                    );
                    return result::success;
                }));
            }
            if(Glib::Variant<Glib::ustring> v; login1->call_sync("CanSuspend", Glib::VariantContainerBase{}).get_child(v), v.get() == "yes") {
                entries.push_back(make_simple<action_menu_entry>("Suspend"_(), config::CONFIG.asset_directory/"icons/icon_action_suspend.png", loader, [this, xmb](){
                    xmb->emplace_overlay<app::message_overlay>("Suspend"_(), "Do you really want to suspend the system?"_(),
                        std::vector<std::string>{"Yes"_(), "No"_()}, [this](unsigned int choice){
                            if(choice == 0) {
                                login1->call_sync("Suspend", Glib::VariantContainerBase::create_tuple(Glib::Variant<bool>::create(true)));
                            }
                        }
                    );
                    return result::success;
                }));
            }
        } catch (const std::exception& e) {
            spdlog::error("Failed to get power management information: {}", static_cast<std::string>(e.what()));
        }
    }
}
