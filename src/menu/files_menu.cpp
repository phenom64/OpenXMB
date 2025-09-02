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
#include <filesystem>
#include <functional>
#include <map>
#include <numeric>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>
#include <fstream>
#include <nlohmann/json.hpp>

#include <unistd.h>

module openxmb.app;

import :files_menu;
import :menu_base;
import :menu_utils;
import :message_overlay;
import :choice_overlay;
import :programs;

import openxmb.config;
import openxmb.utils;
import dreamrender;
import sdl2;
import spdlog;
import i18n;

namespace menu {
    using namespace mfk::i18n::literals;

    // Implementation of file_info constructor
    file_info::file_info(const std::filesystem::directory_entry& entry) {
        name = entry.path().filename().string();
        display_name = name;
        
        try {
            auto status = entry.status();
            is_directory = std::filesystem::is_directory(status);
            is_symlink = std::filesystem::is_symlink(status);
            is_hidden = name[0] == '.';
            
            if (std::filesystem::is_regular_file(status)) {
                size = std::filesystem::file_size(entry.path());
            } else {
                size = 0;
            }
            
            modification_time = entry.last_write_time();
            
            // Determine content type based on file extension
            std::string ext = entry.path().extension().string();
            if (ext.empty()) {
                if (is_directory) {
                    content_type = "inode/directory";
                } else {
                    content_type = "application/octet-stream";
                }
            } else {
                // Common MIME type mappings
                static const std::map<std::string, std::string> mime_types = {
                    {".txt", "text/plain"},
                    {".md", "text/markdown"},
                    {".html", "text/html"},
                    {".htm", "text/html"},
                    {".css", "text/css"},
                    {".js", "application/javascript"},
                    {".json", "application/json"},
                    {".xml", "application/xml"},
                    {".pdf", "application/pdf"},
                    {".jpg", "image/jpeg"},
                    {".jpeg", "image/jpeg"},
                    {".png", "image/png"},
                    {".gif", "image/gif"},
                    {".bmp", "image/bmp"},
                    {".svg", "image/svg+xml"},
                    {".ico", "image/x-icon"},
                    {".mp3", "audio/mpeg"},
                    {".wav", "audio/wav"},
                    {".ogg", "audio/ogg"},
                    {".mp4", "video/mp4"},
                    {".avi", "video/x-msvideo"},
                    {".mkv", "video/x-matroska"},
                    {".mov", "video/quicktime"},
                    {".zip", "application/zip"},
                    {".tar", "application/x-tar"},
                    {".gz", "application/gzip"},
                    {".7z", "application/x-7z-compressed"},
                    {".exe", "application/x-executable"},
                    {".deb", "application/vnd.debian.binary-package"},
                    {".rpm", "application/x-rpm"},
                    {".app", "application/x-executable"},
                    {".dmg", "application/x-apple-diskimage"}
                };
                
                auto it = mime_types.find(ext);
                if (it != mime_types.end()) {
                    content_type = it->second;
                } else {
                    content_type = "application/octet-stream";
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("Error getting file info for {}: {}", entry.path().string(), e.what());
            content_type = "application/octet-stream";
            size = 0;
            is_directory = false;
            is_hidden = false;
            is_symlink = false;
        }
    }

    files_menu::files_menu(std::string name, dreamrender::texture&& icon, app::shell* xmb, std::filesystem::path path, dreamrender::resource_loader& loader)
    : simple_menu(std::move(name), std::move(icon)), xmb(xmb), path(std::move(path)), loader(loader)
    {
        reload();
    }

    void files_menu::reload() {
        if(selected_submenu < extra_data_entries.size()) {
            old_selected_item = extra_data_entries[selected_submenu].path;
        }

        entries.clear();
        extra_data_entries.clear();

        try {
            std::filesystem::directory_iterator it{path};
            std::vector<file_info> file_infos;

            for (const auto& entry : it) {
                try {
                    file_info info(entry);
                    
                    if(!filter(info)) {
                        continue;
                    }

                    file_infos.push_back(info);
                } catch (const std::exception& e) {
                    spdlog::warn("Error processing file {}: {}", entry.path().string(), e.what());
                }
            }

            // Sort files
            std::sort(file_infos.begin(), file_infos.end(), [this](const file_info& a, const file_info& b) {
                bool result = sort(a, b);
                return sort_descending ? !result : result;
            });

            // Create menu entries
            for (const auto& info : file_infos) {
                extra_data_entries.push_back({path / info.name, info});
                
                std::string icon_path;
                std::string extension = std::filesystem::path(info.name).extension().string();
                
                // Try to find appropriate icon
                if (info.content_type.starts_with("image/") || 
                    extension == ".png" || extension == ".jpg" || extension == ".jpeg" || 
                    extension == ".bmp" || extension == ".gif") {
                    icon_path = (path / info.name).string();
                } else {
                    // Try to resolve icon from JSON config
                    if(auto r = utils::resolve_icon_from_json(info.content_type)) {
                        icon_path = r->string();
                    }
                }

                dreamrender::texture icon_texture(loader.getDevice(), loader.getAllocator());
                auto entry = std::make_unique<action_menu_entry>(
                    info.display_name, 
                    std::move(icon_texture),
                    std::function<result()>{}, 
                    [this, info](action a) { 
                        return activate_file(info, a); 
                    }
                );
                
                if(!icon_path.empty()) {
                    try {
                        loader.loadTexture(&entry->get_icon(), icon_path);
                    } catch (const std::exception& e) {
                        spdlog::debug("Failed to load icon for {}: {}", info.name, e.what());
                    }
                }
                
                entries.push_back(std::move(entry));
            }
        } catch (const std::exception& e) {
            spdlog::error("Error reloading files menu: {}", e.what());
        }
    }

    result files_menu::activate_file(const file_info& info, action action) {
        if(action == action::ok) {
            std::filesystem::path file_path = path / info.name;
            
            if (info.is_directory) {
                // Navigate into directory within the same menu
                path = file_path;
                reload();
                return result::success;
            } else {
                // Try to open file with appropriate program
                auto open_infos = programs::get_open_infos(file_path, programs::file_info(file_path));
                if (!open_infos.empty()) {
                    auto& open_info = open_infos[0];
                    auto component = open_info.create(file_path, loader);
                    if (component) {
                        xmb->push_overlay(std::move(component));
                        return result::submenu;
                    }
                }
                
                // Fallback: show message that no program can open this file
                xmb->emplace_overlay<app::message_overlay>(
                    "Cannot Open File"_(),
                    "No suitable program found to open this file type."_()
                );
                return result::close;
            }
        }
        return result::unsupported;
    }

    result files_menu::activate(action action) {
        if(action == action::extra) {
            selected_filter = (selected_filter + 1) % filters.size();
            filter = filters[selected_filter].second;
            reload();
            return result::unsupported;
        } else if(action == action::options) {
            selected_sort = (selected_sort + 1) % sorts.size();
            sort = sorts[selected_sort].second;
            reload();
            return result::unsupported;
        } else if(action == action::cancel) {
            sort_descending = !sort_descending;
            reload();
            return result::unsupported;
        }
        return simple_menu::activate(action);
    }

    void files_menu::get_button_actions(std::vector<std::pair<action, std::string>>& v) {
        v.emplace_back(action::none, "");
        v.emplace_back(action::none, "");
        v.emplace_back(action::extra, filters[selected_filter].first);
        v.emplace_back(action::options, sorts[selected_sort].first);
        v.emplace_back(action::cancel, sort_descending ? "Ascending"_() : "Descending"_());
    }

    void files_menu::on_open() {
        simple_menu::on_open();
        reload();
    }

    void files_menu::resort() {
        reload();
    }

}
