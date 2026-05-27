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
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
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
import :menu_utils;

namespace menu {

using namespace mfk::i18n::literals;

namespace {

bool desktop_bool(std::string value)
{
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value == "true" || value == "1";
}

std::vector<std::string> split_desktop_exec(const std::string& command_line)
{
    std::vector<std::string> tokens;
    std::string token;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;

    auto flush = [&]() {
        if(!token.empty()) {
            tokens.push_back(std::move(token));
            token.clear();
        }
    };

    for(char c : command_line) {
        if(escaped) {
            token.push_back(c);
            escaped = false;
            continue;
        }

        if(c == '\\') {
            escaped = true;
            continue;
        }

        if(c == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            continue;
        }

        if(c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            continue;
        }

        if(std::isspace(static_cast<unsigned char>(c)) && !in_single_quote && !in_double_quote) {
            flush();
            continue;
        }

        token.push_back(c);
    }

    if(escaped) {
        token.push_back('\\');
    }
    flush();
    return tokens;
}

std::optional<std::vector<std::string>> expand_desktop_exec_token(const std::string& token, const app_info& app)
{
    if(token == "%i") {
        if(app.icon.empty()) {
            return std::vector<std::string>{};
        }
        return std::vector<std::string>{"--icon", app.icon};
    }

    std::string expanded;
    expanded.reserve(token.size());
    for(std::size_t i = 0; i < token.size(); ++i) {
        if(token[i] != '%') {
            expanded.push_back(token[i]);
            continue;
        }

        if(i + 1 >= token.size()) {
            return std::nullopt;
        }

        char code = token[++i];
        switch(code) {
            case '%':
                expanded.push_back('%');
                break;
            case 'c':
                expanded += app.name;
                break;
            case 'k':
                expanded += app.desktop_file.string();
                break;
            case 'i':
                expanded += app.icon;
                break;
            case 'f':
            case 'F':
            case 'u':
            case 'U':
            case 'd':
            case 'D':
            case 'n':
            case 'N':
            case 'v':
            case 'm':
                return std::nullopt;
            default:
                return std::nullopt;
        }
    }

    if(expanded.empty()) {
        return std::vector<std::string>{};
    }
    return std::vector<std::string>{std::move(expanded)};
}

std::vector<std::string> build_launch_args(const app_info& app)
{
    std::vector<std::string> args;
    for(const auto& token : split_desktop_exec(app.exec)) {
        auto expanded = expand_desktop_exec_token(token, app);
        if(!expanded) {
            continue;
        }
        args.insert(args.end(), expanded->begin(), expanded->end());
    }

    if(app.terminal) {
        return terminal_command(std::move(args));
    }
    return args;
}

} // namespace

// Parse desktop file to extract application info
app_info::app_info(const std::filesystem::path& desktop_file) {
    this->desktop_file = desktop_file;
    id = desktop_file.stem().string();
    name = id;
    comment = "";
    exec = "";
    icon = "";
    categories = "";
    terminal = false;
    hidden = false;
    no_display = false;
    
    if (!std::filesystem::exists(desktop_file)) {
        return;
    }
    
    try {
        std::ifstream file(desktop_file);
        std::string line;
        bool in_desktop_entry = false;
        bool unsupported_type = false;
        
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
            
            if (key == "Type" && value != "Application") {
                unsupported_type = true;
            } else if (key == "Name") {
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
                terminal = desktop_bool(value);
            } else if (key == "Hidden") {
                hidden = desktop_bool(value);
            } else if (key == "NoDisplay") {
                no_display = desktop_bool(value);
            }
        }
        hidden = hidden || unsupported_type;
    } catch (const std::exception& e) {
        spdlog::warn("Failed to parse desktop file {}: {}", desktop_file.string(), e.what());
    }
}

applications_menu::applications_menu(std::string name, dreamrender::texture&& icon, app::shell* xmb, dreamrender::resource_loader& loader, AppFilter filter)
    : simple_menu(std::move(name), std::move(icon)), xmb(xmb), loader(loader), filter(filter)
{
    auto scanned_apps = scan_applications();
    for (const auto& app : scanned_apps) {
        if(!filter(app))
            continue;
        bool is_hidden = app.no_display || config::CONFIG.excludedApplications.contains(app.id);
        if(!show_hidden && is_hidden)
            continue;

        spdlog::trace("Found application: {} ({})", app.name, app.id);
        auto entry = create_action_menu_entry(app, is_hidden);
        apps.push_back(app);
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
    std::unordered_set<std::string> seen_ids;
    
    // Common desktop file locations
    std::vector<std::filesystem::path> search_paths = {
        std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "") / ".local/share/applications",
        "/usr/share/applications",
        "/usr/local/share/applications",
        "/opt/applications"
    };
    
    for (const auto& search_path : search_paths) {
        if (!std::filesystem::exists(search_path)) {
            continue;
        }
        
        try {
            for (const auto& entry : std::filesystem::directory_iterator(search_path)) {
                if (entry.path().extension() == ".desktop") {
                    app_info app(entry.path());
                    if (!app.name.empty() && !app.exec.empty() && !app.hidden && !seen_ids.contains(app.id)) {
                        seen_ids.insert(app.id);
                        app_list.push_back(app);
                    }
                }
            }
        } catch (const std::exception& e) {
            spdlog::debug("Error scanning directory {}: {}", search_path.string(), e.what());
        }
    }

    std::ranges::sort(app_list, [](const app_info& a, const app_info& b) {
        return a.name < b.name;
    });
    
    return app_list;
}

void applications_menu::reload() {
    std::string selected_app_id;
    if(selected_submenu < apps.size()) {
        selected_app_id = apps[selected_submenu].id;
    }

    auto scanned_apps = scan_applications();
    apps.clear();
    entries.clear();
    
    for (const auto& app : scanned_apps) {
        if(!filter(app))
            continue;
        bool is_hidden = app.no_display || config::CONFIG.excludedApplications.contains(app.id);
        if(!show_hidden && is_hidden)
            continue;

        spdlog::trace("Found application: {} ({})", app.name, app.id);
        auto entry = create_action_menu_entry(app, is_hidden);
        apps.push_back(app);
        entries.push_back(std::move(entry));
    }

    selected_submenu = 0;
    if(!selected_app_id.empty()) {
        if(auto it = std::ranges::find_if(apps, [&selected_app_id](const app_info& app) {
            return app.id == selected_app_id;
        }); it != apps.end()) {
            selected_submenu = static_cast<unsigned int>(std::distance(apps.begin(), it));
        }
    }
}

result applications_menu::activate_app(const app_info& app, action action) {
    if(action == action::ok) {
        auto args = build_launch_args(app);
        if(args.empty()) {
            spdlog::warn("Could not build launch command for application: {}", app.name);
            return result::failure;
        }

        if(!launch_detached(args)) {
            spdlog::warn("Failed to launch application: {} ({})", app.name, app.exec);
            return result::failure;
        }

        return result::success;
    } else if(action == action::options) {
        bool hidden = config::CONFIG.excludedApplications.contains(app.id);
        xmb->emplace_overlay<app::choice_overlay>(std::vector{
            "Launch Application"_(), "View information"_(), hidden ? "Show in XMB"_() : "Hide from XMB"_()
        }, 0, [this, app, hidden](unsigned int index){
            switch(index) {
                case 0:
                    return activate_app(app, action::ok);
                case 1: {
                    auto args = build_launch_args(app);
                    std::string command_line;
                    for(const auto& arg : args) {
                        if(!command_line.empty()) {
                            command_line += " ";
                        }
                        command_line += arg;
                    }
                    xmb->emplace_overlay<app::message_overlay>(
                        "Application Information"_(),
                        std::string("Name: ") + app.name + "\n" +
                        std::string("ID: ") + app.id + "\n" +
                        std::string("Exec: ") + app.exec + "\n" +
                        std::string("Command: ") + command_line + "\n" +
                        std::string("Categories: ") + app.categories + "\n" +
                        std::string("Terminal: ") + (app.terminal ? "Yes" : "No") + "\n" +
                        std::string("Desktop file: ") + app.desktop_file.string()
                    );
                    return result::close;
                }
                case 2:
                    if(hidden) {
                        config::CONFIG.excludeApplication(app.id, false);
                        config::CONFIG.save_config();
                        reload();
                    } else {
                        xmb->emplace_overlay<app::message_overlay>(
                            "Hide Application"_(),
                            "Are you sure you want to hide this application from OpenXMB?"_(),
                            std::vector<std::string>{"Yes"_(), "No"_()},
                            [this, app](unsigned int choice) {
                                if(choice == 0) {
                                    config::CONFIG.excludeApplication(app.id);
                                    config::CONFIG.save_config();
                                    reload();
                                }
                            },
                            true
                        );
                    }
                    return result::close;
                default:
                    return result::unsupported;
            }
        });
        return result::success;
    }
    return result::unsupported;
}

result applications_menu::activate(action action) {
    if(action == action::extra) {
        show_hidden = !show_hidden;
        reload();
        return result::success;
    }
    return simple_menu::activate(action);
}

void applications_menu::get_button_actions(std::vector<std::pair<action, std::string>>& v) {
    if(!v.empty()) {
        return;
    }
    v.emplace_back(action::none, "");
    v.emplace_back(action::none, "");
    v.emplace_back(action::options, "Options"_());
    v.emplace_back(action::extra, show_hidden ? "Hide excluded apps"_() : "Show excluded apps"_());
}

}
