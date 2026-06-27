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
#if !defined(_WIN32)
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#endif

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

#if !defined(_WIN32)
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
#endif

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
        std::string_view compatibility_icon_name,
        std::string_view clean_icon_name,
        dreamrender::resource_loader& loader,
        std::function<result()> callback
    ) {
        dreamrender::texture icon_texture(loader.getDevice(), loader.getAllocator());
        auto entry = std::make_unique<action_menu_entry>(
            label,
            std::move(icon_texture),
            std::move(callback)
        );
        const auto compatibility_icon = config::CONFIG.asset_directory /
            "compat/xmb-ui-compat/icons" / compatibility_icon_name;
        if(!load_icon_if_present(loader, *entry, compatibility_icon)) {
            const auto clean_icon = config::CONFIG.asset_directory / "icons" / clean_icon_name;
            spdlog::debug("Compatibility icon {} is unavailable; using clean icon {}",
                compatibility_icon.string(), clean_icon.string());
            load_icon_if_present(loader, *entry, clean_icon);
        }
        return entry;
    }

    } // namespace

    user_info::user_info(const std::string& name) : username(name) {
#if defined(_WIN32)
        real_name = name;
        if(const char* profile = std::getenv("USERPROFILE"); profile != nullptr) {
            home_directory = profile;
        } else {
            home_directory = "";
        }
        shell = "";
        uid = 0;
        gid = 0;
        is_active = true;
        is_admin = false;
#else
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
#endif
    }

    users_menu::users_menu(std::string name, dreamrender::texture&& icon, app::shell* xmb, dreamrender::resource_loader& loader)
        : simple_menu(std::move(name), std::move(icon)), xmb(xmb), loader(loader)
    {
        reload();
    }

    std::vector<user_info> users_menu::scan_users() {
        std::vector<user_info> user_list;

#if defined(_WIN32)
        if(const char* username = std::getenv("USERNAME"); username != nullptr && *username != '\0') {
            user_list.emplace_back(username);
        }
        return user_list;
#else
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
#endif
    }

    void users_menu::reload() {
        std::string selected_name;
        if(selected_submenu < entries.size()) {
            selected_name = std::string(entries[selected_submenu]->get_name());
        }

        entries.clear();
        users = scan_users();

        // Keep the measured reference rail stable. The first item exits only
        // after the shell's existing confirmation path; it never calls an OS
        // shutdown command directly.
        entries.push_back(make_user_action_entry(
            "Turn Off System"_(), "xmb_icon_054.png", "icon_action_quit.png", loader, [this]() {
            xmb->emplace_overlay<app::message_overlay>(
                "Turn Off System"_(),
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

        entries.push_back(make_user_action_entry(
            "Create New User"_(), "xmb_icon_041.png", "icon_category_users.png", loader, [this]() {
                xmb->emplace_overlay<app::message_overlay>(
                    "Create New User"_(),
                    "User creation is not available on this system."_()
                );
                return result::success;
            }));

        dreamrender::texture current_user_icon(loader.getDevice(), loader.getAllocator());
        auto current_user = std::make_unique<action_menu_entry>(
            "*User"_(),
            std::move(current_user_icon),
            std::function<result()>{},
            [this](action user_action) {
                if(users.empty()) {
                    if(user_action == action::ok || user_action == action::options) {
                        xmb->emplace_overlay<app::message_overlay>(
                            "User Information"_(),
                            "No local user information is available."_()
                        );
                        return result::success;
                    }
                    return result::unsupported;
                }
                return activate_user(users.front(), user_action);
            }
        );
        const auto compatibility_user_icon = config::CONFIG.asset_directory /
            "compat/xmb-ui-compat/icons/xmb_icon_041.png";
        if(!load_icon_if_present(loader, *current_user, compatibility_user_icon)) {
            load_icon_if_present(loader, *current_user,
                config::CONFIG.asset_directory / "icons/icon_category_users.png");
        }
        entries.push_back(std::move(current_user));

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
