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
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>

module openxmb.app;

import :users_menu;
import :menu_base;
import :menu_utils;
import :message_overlay;
import :choice_overlay;
import sdl2;
import openxmb.config;
import openxmb.utils;
import dreamrender;
import spdlog;
import i18n;

namespace menu {
    using namespace mfk::i18n::literals;

    namespace {

    std::string clean_gecos(const char* gecos, const std::string& fallback)
    {
        if(gecos == nullptr || *gecos == '\0') {
            return fallback;
        }

        std::string value{gecos};
        if(auto comma = value.find(','); comma != std::string::npos) {
            value.erase(comma);
        }
        return value.empty() ? fallback : value;
    }

    bool group_contains_user(const char* group_name, const std::string& username, gid_t primary_gid)
    {
        auto* group = getgrnam(group_name);
        if(group == nullptr) {
            return false;
        }

        if(group->gr_gid == primary_gid) {
            return true;
        }

        if(group->gr_mem == nullptr) {
            return false;
        }

        for(int i = 0; group->gr_mem[i] != nullptr; ++i) {
            if(username == group->gr_mem[i]) {
                return true;
            }
        }
        return false;
    }

    bool is_admin_user(const std::string& username, gid_t primary_gid)
    {
        return group_contains_user("wheel", username, primary_gid) ||
            group_contains_user("sudo", username, primary_gid) ||
            group_contains_user("admin", username, primary_gid);
    }

    bool is_login_shell(const char* shell)
    {
        if(shell == nullptr || *shell == '\0') {
            return false;
        }

        std::string shell_name = std::filesystem::path(shell).filename().string();
        return shell_name != "false" && shell_name != "nologin";
    }

    bool load_icon_if_present(dreamrender::resource_loader& loader, action_menu_entry& entry, const std::filesystem::path& path)
    {
        std::error_code ec;
        if(!std::filesystem::exists(path, ec) || ec) {
            return false;
        }

        try {
            loader.loadTexture(&entry.get_icon(), path);
            return true;
        } catch(const std::exception& e) {
            spdlog::debug("Failed to load user menu icon {}: {}", path.string(), e.what());
            return false;
        }
    }

    std::unique_ptr<action_menu_entry> make_user_action_entry(
        const std::string& label,
        const std::string& icon_name,
        dreamrender::resource_loader& loader,
        std::function<result()> callback
    ) {
        dreamrender::texture icon_texture(loader.getDevice(), loader.getAllocator());
        auto entry = std::make_unique<action_menu_entry>(
            label,
            std::move(icon_texture),
            std::move(callback)
        );
        load_icon_if_present(loader, *entry, config::CONFIG.asset_directory / "icons" / icon_name);
        return entry;
    }

    void confirm_and_launch(app::shell* xmb, const std::string& title, const std::string& message, std::vector<std::string> args)
    {
        xmb->emplace_overlay<app::message_overlay>(
            title,
            message,
            std::vector<std::string>{"Yes"_(), "No"_()},
            [xmb, title, args = std::move(args)](unsigned int choice) {
                if(choice != 0) {
                    return;
                }
                if(!launch_detached(args)) {
                    xmb->emplace_overlay<app::message_overlay>(
                        title,
                        "The requested system action could not be started."_()
                    );
                }
            },
            true
        );
    }

    bool add_guarded_system_actions(
        std::vector<std::unique_ptr<menu_entry>>& entries,
        app::shell* xmb,
        dreamrender::resource_loader& loader
    ) {
        bool added = false;

#if defined(__linux__)
        if(command_available("loginctl")) {
            entries.push_back(make_user_action_entry("Lock Screen"_(), "icon_action_lock.png", loader, [xmb]() {
                if(!launch_detached({"loginctl", "lock-session"})) {
                    xmb->emplace_overlay<app::message_overlay>("Lock Screen"_(), "The session could not be locked."_());
                }
                return result::success;
            }));
            added = true;

            if(const char* session = std::getenv("XDG_SESSION_ID"); session != nullptr && *session != '\0') {
                entries.push_back(make_user_action_entry("Log Out"_(), "icon_action_logout.png", loader, [xmb, session_id = std::string(session)]() {
                    confirm_and_launch(
                        xmb,
                        "Log Out"_(),
                        "Do you really want to log out of this session?"_(),
                        {"loginctl", "terminate-session", session_id}
                    );
                    return result::success;
                }));
                added = true;
            }
        }

        if(command_available("systemctl")) {
            entries.push_back(make_user_action_entry("Suspend"_(), "icon_action_suspend.png", loader, [xmb]() {
                confirm_and_launch(
                    xmb,
                    "Suspend"_(),
                    "Do you really want to suspend the system?"_(),
                    {"systemctl", "suspend"}
                );
                return result::success;
            }));
            entries.push_back(make_user_action_entry("Reboot"_(), "icon_action_reboot.png", loader, [xmb]() {
                confirm_and_launch(
                    xmb,
                    "Reboot"_(),
                    "Do you really want to reboot the system?"_(),
                    {"systemctl", "reboot"}
                );
                return result::success;
            }));
            entries.push_back(make_user_action_entry("Power off"_(), "icon_action_poweroff.png", loader, [xmb]() {
                confirm_and_launch(
                    xmb,
                    "Power off"_(),
                    "Do you really want to power off the system?"_(),
                    {"systemctl", "poweroff"}
                );
                return result::success;
            }));
            added = true;
        }
#elif defined(__APPLE__)
        const std::filesystem::path cg_session = "/System/Library/CoreServices/Menu Extras/User.menu/Contents/Resources/CGSession";
        std::error_code ec;
        if(std::filesystem::exists(cg_session, ec) && !ec) {
            entries.push_back(make_user_action_entry("Lock Screen"_(), "icon_action_lock.png", loader, [xmb, cg_session]() {
                if(!launch_detached({cg_session.string(), "-suspend"})) {
                    xmb->emplace_overlay<app::message_overlay>("Lock Screen"_(), "The session could not be locked."_());
                }
                return result::success;
            }));
            added = true;
        }

        if(command_available("osascript")) {
            entries.push_back(make_user_action_entry("Log Out"_(), "icon_action_logout.png", loader, [xmb]() {
                confirm_and_launch(
                    xmb,
                    "Log Out"_(),
                    "Do you really want to log out of this session?"_(),
                    {"osascript", "-e", "tell application \"System Events\" to log out"}
                );
                return result::success;
            }));
            entries.push_back(make_user_action_entry("Sleep"_(), "icon_action_suspend.png", loader, [xmb]() {
                confirm_and_launch(
                    xmb,
                    "Sleep"_(),
                    "Do you really want to sleep the system?"_(),
                    {"osascript", "-e", "tell application \"System Events\" to sleep"}
                );
                return result::success;
            }));
            entries.push_back(make_user_action_entry("Reboot"_(), "icon_action_reboot.png", loader, [xmb]() {
                confirm_and_launch(
                    xmb,
                    "Reboot"_(),
                    "Do you really want to reboot the system?"_(),
                    {"osascript", "-e", "tell application \"System Events\" to restart"}
                );
                return result::success;
            }));
            entries.push_back(make_user_action_entry("Power off"_(), "icon_action_poweroff.png", loader, [xmb]() {
                confirm_and_launch(
                    xmb,
                    "Power off"_(),
                    "Do you really want to power off the system?"_(),
                    {"osascript", "-e", "tell application \"System Events\" to shut down"}
                );
                return result::success;
            }));
            added = true;
        }
#endif

        return added;
    }

    } // namespace

    user_info::user_info(const std::string& name) : username(name) {
        struct passwd* pwd = getpwnam(name.c_str());
        if (pwd) {
            real_name = clean_gecos(pwd->pw_gecos, name);
            home_directory = pwd->pw_dir ? pwd->pw_dir : "";
            shell = pwd->pw_shell ? pwd->pw_shell : "";
            uid = static_cast<std::uint64_t>(pwd->pw_uid);
            gid = static_cast<std::uint64_t>(pwd->pw_gid);
            is_active = is_login_shell(pwd->pw_shell);
            is_admin = is_admin_user(name, pwd->pw_gid);
        } else {
            real_name = name;
            home_directory = "";
            shell = "";
            uid = 0;
            gid = 0;
            is_active = false;
            is_admin = false;
        }
    }

    users_menu::users_menu(std::string name, dreamrender::texture&& icon, app::shell* xmb, dreamrender::resource_loader& loader)
        : simple_menu(std::move(name), std::move(icon)), xmb(xmb), loader(loader)
    {
        reload();
    }

    std::vector<user_info> users_menu::scan_users() {
        std::vector<user_info> user_list;

        try {
            constexpr uid_t min_user_uid =
#if defined(__APPLE__)
                500;
#else
                1000;
#endif

            setpwent();
            while (auto* pwd = getpwent()) {
                if(pwd->pw_uid < min_user_uid || !is_login_shell(pwd->pw_shell)) {
                    continue;
                }

                user_info user(pwd->pw_name ? pwd->pw_name : "");
                if (user.is_active && !user.username.empty()) {
                    user_list.push_back(user);
                }
            }
            endpwent();
        } catch (const std::exception& e) {
            spdlog::warn("Error scanning users: {}", e.what());
            endpwent();
        }

        auto my_uid = static_cast<std::uint64_t>(getuid());
        // Sort users by username
        std::sort(user_list.begin(), user_list.end(), [my_uid](const user_info& a, const user_info& b) {
            if(a.uid == my_uid && b.uid != my_uid) {
                return true;
            }
            if(a.uid != my_uid && b.uid == my_uid) {
                return false;
            }
            return a.username < b.username;
        });

        return user_list;
    }

    void users_menu::reload() {
        std::string selected_name;
        if(selected_submenu < entries.size()) {
            selected_name = std::string(entries[selected_submenu]->get_name());
        }

        entries.clear();
        users = scan_users();
        
        for (const auto& user : users) {
            dreamrender::texture icon_texture(loader.getDevice(), loader.getAllocator());
            
            std::string display_name = user.username;
            if (user.is_admin) {
                display_name += " (Admin)";
            }
            
            auto entry = std::make_unique<action_menu_entry>(
                display_name, 
                std::move(icon_texture),
                std::function<result()>{}, 
                [this, user](action a) { 
                    return activate_user(user, a); 
                }
            );
            
            entries.push_back(std::move(entry));
        }

        // Add Quit option (PS3-style behavior under Users column)
        entries.push_back(make_user_action_entry("Quit OpenXMB"_(), "icon_action_quit.png", loader, [this]() {
            xmb->emplace_overlay<app::message_overlay>(
                "Quit OpenXMB"_(),
                "Do you want to quit OpenXMB?"_(),
                std::vector<std::string>{"Yes"_(), "No"_()},
                [](unsigned int idx) {
                    if(idx == 0) {
                        sdl::Event e{};
                        e.type = sdl::EventType::SDL_QUIT;
                        sdl::PushEvent(&e);
                    }
                },
                true
            );
            return result::success;
        }));

        add_guarded_system_actions(entries, xmb, loader);

        selected_submenu = 0;
        if(!selected_name.empty()) {
            if(auto it = std::ranges::find_if(entries, [&selected_name](const std::unique_ptr<menu_entry>& entry) {
                return std::string_view{entry->get_name()} == selected_name;
            }); it != entries.end()) {
                selected_submenu = static_cast<unsigned int>(std::distance(entries.begin(), it));
            }
        }
    }

    result users_menu::activate_user(const user_info& user, action action) {
        if (action == action::ok) {
            // Show user information
            std::string info = "Username: " + user.username + "\n";
            info += "Real Name: " + user.real_name + "\n";
            info += "UID: " + std::to_string(user.uid) + "\n";
            info += "GID: " + std::to_string(user.gid) + "\n";
            info += "Home Directory: " + user.home_directory + "\n";
            info += "Shell: " + user.shell + "\n";
            info += "Status: " + std::string(user.is_active ? "Active" : "Inactive") + "\n";
            info += "Role: " + std::string(user.is_admin ? "Administrator" : "User");
            
            xmb->emplace_overlay<app::message_overlay>(
                "User Information"_(),
                info
            );
            return result::success;
        } else if (action == action::options) {
            // Show user options
            std::vector<std::string> options = {
                "View Information"_(),
                "Switch User"_(),
                "Change Password"_()
            };
            
            xmb->emplace_overlay<app::choice_overlay>(
                options, 0, [this, user](unsigned int index) {
                    switch (index) {
                        case 0: // View Information
                            return activate_user(user, action::ok);
                        case 1: { // Switch User
#if defined(__linux__)
                            if(command_available("dm-tool")) {
                                if(!launch_detached({"dm-tool", "switch-to-user", user.username})) {
                                    xmb->emplace_overlay<app::message_overlay>(
                                        "Switch User"_(),
                                        "User switching could not be started."_()
                                    );
                                }
                            } else
#elif defined(__APPLE__)
                            if(command_available("osascript")) {
                                if(!launch_detached({"osascript", "-e", "tell application \"System Events\" to keystroke \"q\" using {control down, command down}"})) {
                                    xmb->emplace_overlay<app::message_overlay>(
                                        "Switch User"_(),
                                        "User switching could not be started."_()
                                    );
                                }
                            } else
#endif
                            {
                                xmb->emplace_overlay<app::message_overlay>(
                                    "Not Available"_(),
                                    "User switching is not available on this system."_()
                                );
                            }
                            return result::close;
                        }
                        case 2: { // Change Password
#if defined(_WIN32)
                            xmb->emplace_overlay<app::message_overlay>(
                                "Not Available"_(),
                                "Password changing is not available on this system."_()
                            );
#else
                            if(auto args = terminal_command({"passwd", user.username}); !args.empty()) {
                                if(!launch_detached(args)) {
                                    xmb->emplace_overlay<app::message_overlay>(
                                        "Change Password"_(),
                                        "Password changing could not be started."_()
                                    );
                                }
                            } else {
                                xmb->emplace_overlay<app::message_overlay>(
                                    "Not Available"_(),
                                    "No terminal emulator is available to run passwd."_()
                                );
                            }
#endif
                            return result::close;
                        }
                        default:
                            return result::unsupported;
                    }
                }
            );
            return result::success;
        }
        return result::unsupported;
    }

    result users_menu::activate(action action) {
        if (action == action::extra) {
            reload();
            return result::success;
        }
        return simple_menu::activate(action);
    }

    void users_menu::get_button_actions(std::vector<std::pair<action, std::string>>& v) {
        if(!v.empty()) {
            return;
        }
        v.emplace_back(action::none, "");
        v.emplace_back(action::none, "");
        v.emplace_back(action::options, "Options"_());
        v.emplace_back(action::extra, "Refresh"_());
    }

}
