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
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

module openxmb.app;

import spdlog;
import i18n;
import dreamrender;
import openxmb.config;

import :applications_menu;
import :choice_overlay;
import :message_overlay;

namespace menu {

using namespace mfk::i18n::literals;

// Parse desktop file to extract application info
app_info::app_info(const std::filesystem::path& desktop_file) {
    id = desktop_file.stem().string();
    name = id;
    comment = "";
    exec = "";
    icon = "";
    categories = "";
    terminal = false;
    hidden = false;
    
    if (!std::filesystem::exists(desktop_file)) {
        return;
    }
    
    try {
        std::ifstream file(desktop_file);
        std::string line;
        bool in_desktop_entry = false;
        
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            
            if (line == "[Desktop Entry]") {
                in_desktop_entry = true;
                continue;
            }
            
            if (!in_desktop_entry) {
                continue;
            }
            
            size_t pos = line.find('=');
            if (pos == std::string::npos) {
                continue;
            }
            
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            
            if (key == "Name") {
                name = value;
            } else if (key == "Comment") {
                comment = value;
            } else if (key == "Exec") {
                exec = value;
            } else if (key == "Icon") {
                icon = value;
            } else if (key == "Categories") {
                categories = value;
            } else if (key == "Terminal") {
                terminal = (value == "true");
            } else if (key == "Hidden") {
                hidden = (value == "true");
            }
        }
    } catch (const std::exception& e) {
        spdlog::warn("Failed to parse desktop file {}: {}", desktop_file.string(), e.what());
    }
}

applications_menu::applications_menu(std::string name, dreamrender::texture&& icon, app::shell* xmb, dreamrender::resource_loader& loader, AppFilter filter)
    : simple_menu(std::move(name), std::move(icon)), xmb(xmb), loader(loader), filter(filter)
{
    apps = scan_applications();
    for (const auto& app : apps) {
        if(!filter(app))
            continue;
        bool is_hidden = config::CONFIG.excludedApplications.contains(app.id);
        if(!show_hidden && is_hidden)
            continue;

        spdlog::trace("Found application: {} ({})", app.name, app.id);
        auto entry = create_action_menu_entry(app, is_hidden);
        entries.push_back(std::move(entry));
    }
}

std::unique_ptr<action_menu_entry> applications_menu::create_action_menu_entry(const app_info& app, bool hidden) {
    std::string icon_path;
    if(auto r = utils::resolve_icon_from_json(app.icon)) {
        icon_path = r->string();
    } else {
        spdlog::warn("Could not resolve icon for application: {}", app.name);
    }

    dreamrender::texture icon_texture(loader.getDevice(), loader.getAllocator());
    std::string name = app.name;
    if(hidden) {
        name += " (hidden)"_();
    }
    auto entry = std::make_unique<action_menu_entry>(name, std::move(icon_texture),
        std::function<result()>{}, [this, app](action a) { return activate_app(app, a); });
    if(!icon_path.empty()) {
        loader.loadTexture(&entry->get_icon(), icon_path);
    }
    return entry;
}

std::vector<app_info> applications_menu::scan_applications() {
    std::vector<app_info> app_list;
    
    // Common desktop file locations
    std::vector<std::filesystem::path> search_paths = {
        "/usr/share/applications",
        "/usr/local/share/applications",
        "/opt/applications",
        std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "") / ".local/share/applications"
    };
    
    for (const auto& search_path : search_paths) {
        if (!std::filesystem::exists(search_path)) {
            continue;
        }
        
        try {
            for (const auto& entry : std::filesystem::directory_iterator(search_path)) {
                if (entry.path().extension() == ".desktop") {
                    app_info app(entry.path());
                    if (!app.name.empty() && !app.exec.empty()) {
                        app_list.push_back(app);
                    }
                }
            }
        } catch (const std::exception& e) {
            spdlog::debug("Error scanning directory {}: {}", search_path.string(), e.what());
        }
    }
    
    return app_list;
}

void applications_menu::reload() {
    apps = scan_applications();
    entries.clear();
    
    for (const auto& app : apps) {
        if(!filter(app))
            continue;
        bool is_hidden = config::CONFIG.excludedApplications.contains(app.id);
        if(!show_hidden && is_hidden)
            continue;

        spdlog::trace("Found application: {} ({})", app.name, app.id);
        auto entry = create_action_menu_entry(app, is_hidden);
        entries.push_back(std::move(entry));
    }
}

result applications_menu::activate_app(const app_info& app, action action) {
    if(action == action::ok) {
        // Launch application using system command
        std::string command = app.exec;
        if (app.terminal) {
            command = "x-terminal-emulator -e " + command;
        }
        
        int result = system(command.c_str());
        return (result == 0) ? result::success : result::failure;
    } else if(action == action::options) {
        bool hidden = config::CONFIG.excludedApplications.contains(app.id);
        xmb->emplace_overlay<app::choice_overlay>(std::vector{
            "Launch Application"_(), "View information"_(), hidden ? "Show in XMB"_() : "Hide from XMB"_()
        }, 0, [this, app, hidden](unsigned int index){
            switch(index) {
                case 0:
                    return activate_app(app, action::ok);
                case 1:
                    xmb->emplace_overlay<app::message_overlay>(
                        "Application Information"_(),
                        std::string("Name: ") + app.name + "\n" +
                        std::string("ID: ") + app.id + "\n" +
                        std::string("Exec: ") + app.exec + "\n" +
                        std::string("Categories: ") + app.categories + "\n" +
                        std::string("Terminal: ") + (app.terminal ? "Yes" : "No")
                    );
                    return result::close;
                case 2:
                    if(hidden) {
                        config::CONFIG.excludedApplications.erase(app.id);
                    } else {
                        config::CONFIG.excludedApplications.insert(app.id);
                    }
                    config::CONFIG.save_config();
                    reload();
                    return result::close;
                default:
                    return result::unsupported;
            }
        });
        return result::submenu;
    }
    return result::unsupported;
}

result applications_menu::activate(action action) {
    if(action == action::options) {
        show_hidden = !show_hidden;
        reload();
        return result::unsupported;
    }
    return simple_menu::activate(action);
}

void applications_menu::get_button_actions(std::vector<std::pair<action, std::string>>& v) {
    simple_menu::get_button_actions(v);
    v.emplace_back(action::options, show_hidden ? "Hide Hidden"_() : "Show Hidden"_());
}

}
