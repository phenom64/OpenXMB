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
#include <functional>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>

module openxmb.app;

import :users_menu;
import :menu_base;
import :message_overlay;
import :choice_overlay;
import sdl2;
import openxmb.utils;
import dreamrender;
import spdlog;
import i18n;

namespace menu {
    using namespace mfk::i18n::literals;

    user_info::user_info(const std::string& name) : username(name) {
        struct passwd* pwd = getpwnam(name.c_str());
        if (pwd) {
            real_name = pwd->pw_gecos ? pwd->pw_gecos : name;
            home_directory = pwd->pw_dir ? pwd->pw_dir : "";
            shell = pwd->pw_shell ? pwd->pw_shell : "";
            is_active = true;
            
            // Check if user is in admin group (wheel, sudo, admin)
            is_admin = false;
            if (initgroups(name.c_str(), pwd->pw_gid) == 0) {
                if (getgrnam("wheel") && getgrnam("wheel")->gr_mem) {
                    for (int i = 0; getgrnam("wheel")->gr_mem[i]; i++) {
                        if (name == getgrnam("wheel")->gr_mem[i]) {
                            is_admin = true;
                            break;
                        }
                    }
                }
                if (getgrnam("sudo") && getgrnam("sudo")->gr_mem) {
                    for (int i = 0; getgrnam("sudo")->gr_mem[i]; i++) {
                        if (name == getgrnam("sudo")->gr_mem[i]) {
                            is_admin = true;
                            break;
                        }
                    }
                }
                if (getgrnam("admin") && getgrnam("admin")->gr_mem) {
                    for (int i = 0; getgrnam("admin")->gr_mem[i]; i++) {
                        if (name == getgrnam("admin")->gr_mem[i]) {
                            is_admin = true;
                            break;
                        }
                    }
                }
            }
        } else {
            real_name = name;
            home_directory = "";
            shell = "";
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
            // Read /etc/passwd to get user list
            std::ifstream passwd_file("/etc/passwd");
            std::string line;
            
            while (std::getline(passwd_file, line)) {
                size_t pos = line.find(':');
                if (pos != std::string::npos) {
                    std::string username = line.substr(0, pos);
                    
                    // Skip system users (UID < 1000 on most systems)
                    struct passwd* pwd = getpwnam(username.c_str());
                    if (pwd && pwd->pw_uid >= 1000) {
                        user_info user(username);
                        if (user.is_active) {
                            user_list.push_back(user);
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("Error scanning users: {}", e.what());
        }
        
        // Sort users by username
        std::sort(user_list.begin(), user_list.end(), [](const user_info& a, const user_info& b) {
            return a.username < b.username;
        });
        
        return user_list;
    }

    void users_menu::reload() {
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
        {
            dreamrender::texture icon_texture(loader.getDevice(), loader.getAllocator());
            auto quit_entry = std::make_unique<action_menu_entry>(
                std::string{"Quit OpenXMB"_()},
                std::move(icon_texture),
                [this]() {
                    // Confirm quit using a message overlay
                    xmb->emplace_overlay<app::message_overlay>(
                        "Quit OpenXMB"_(),
                        "Do you want to quit OpenXMB?"_(),
                        std::vector<std::string>{"Yes"_(), "No"_()},
                        [](unsigned int idx) {
                            if(idx == 0) {
                                sdl::Event e{}; e.type = sdl::EventType::SDL_QUIT; sdl::PushEvent(&e);
                            }
                        },
                        true
                    );
                    return result::submenu;
                },
                std::function<result(action)>{}
            );
            entries.push_back(std::move(quit_entry));
        }
    }

    result users_menu::activate_user(const user_info& user, action action) {
        if (action == action::ok) {
            // Show user information
            std::string info = "Username: " + user.username + "\n";
            info += "Real Name: " + user.real_name + "\n";
            info += "Home Directory: " + user.home_directory + "\n";
            info += "Shell: " + user.shell + "\n";
            info += "Status: " + std::string(user.is_active ? "Active" : "Inactive") + "\n";
            info += "Role: " + std::string(user.is_admin ? "Administrator" : "User");
            
            xmb->emplace_overlay<app::message_overlay>(
                "User Information"_(),
                info
            );
            return result::close;
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
                        case 1: // Switch User
                            // This would require additional implementation
                            xmb->emplace_overlay<app::message_overlay>(
                                "Not Implemented"_(),
                                "User switching is not yet implemented."_()
                            );
                            return result::close;
                        case 2: // Change Password
                            // This would require additional implementation
                            xmb->emplace_overlay<app::message_overlay>(
                                "Not Implemented"_(),
                                "Password changing is not yet implemented."_()
                            );
                            return result::close;
                        default:
                            return result::unsupported;
                    }
                }
            );
            return result::submenu;
        }
        return result::unsupported;
    }

    result users_menu::activate(action action) {
        if (action == action::extra) {
            reload();
            return result::unsupported;
        }
        return simple_menu::activate(action);
    }

    void users_menu::get_button_actions(std::vector<std::pair<action, std::string>>& v) {
        simple_menu::get_button_actions(v);
        v.emplace_back(action::extra, "Refresh"_());
    }

}
