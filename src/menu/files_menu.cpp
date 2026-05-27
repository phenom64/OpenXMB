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
#include <array>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <thread>
#include <mutex>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>
#include <fstream>
#include <nlohmann/json.hpp>

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

    namespace {

    std::string lower_extension(std::filesystem::path path)
    {
        auto ext = path.extension().string();
        std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return ext;
    }

    std::string format_size(std::uint64_t size)
    {
        const std::array units{"B", "KB", "MB", "GB", "TB"};
        double value = static_cast<double>(size);
        std::size_t unit = 0;
        while(value >= 1024.0 && unit + 1 < units.size()) {
            value /= 1024.0;
            ++unit;
        }

        std::ostringstream ss;
        if(unit == 0) {
            ss << size << " " << units[unit];
        } else {
            ss << std::fixed << std::setprecision(1) << value << " " << units[unit];
        }
        return ss.str();
    }

    std::string format_file_time(std::filesystem::file_time_type time)
    {
        auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
        );
        std::time_t raw_time = std::chrono::system_clock::to_time_t(system_time);
        std::tm local_time{};
#if defined(_WIN32)
        localtime_s(&local_time, &raw_time);
#else
        localtime_r(&raw_time, &local_time);
#endif

        std::ostringstream ss;
        ss << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
        return ss.str();
    }

    std::filesystem::path fallback_icon_for(const file_info& info)
    {
        return config::CONFIG.asset_directory / "icons" / (info.is_directory ? "icon_files_folder.png" : "icon_files_file.png");
    }

    std::filesystem::path icon_for_file(const std::filesystem::path& file_path, const file_info& info)
    {
        auto extension = lower_extension(file_path);
        if(info.content_type.starts_with("image/") ||
            extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
            extension == ".bmp" || extension == ".gif") {
            return file_path;
        }

        if(auto r = utils::resolve_icon_from_json(info.content_type)) {
            return *r;
        }

        return fallback_icon_for(info);
    }

    bool target_inside_source(const std::filesystem::path& src, const std::filesystem::path& dst)
    {
        std::error_code ec;
        auto canonical_src = std::filesystem::weakly_canonical(src, ec);
        if(ec) {
            return false;
        }
        auto canonical_dst = std::filesystem::weakly_canonical(dst, ec);
        if(ec) {
            canonical_dst = std::filesystem::weakly_canonical(dst.parent_path(), ec) / dst.filename();
            if(ec) {
                return false;
            }
        }

        auto src_string = canonical_src.lexically_normal().string();
        auto dst_string = canonical_dst.lexically_normal().string();
        if(dst_string.size() <= src_string.size() || !dst_string.starts_with(src_string)) {
            return false;
        }
        char next = dst_string[src_string.size()];
        return next == std::filesystem::path::preferred_separator || next == '/';
    }

    bool copy_path(app::shell* xmb, const std::filesystem::path& src, const std::filesystem::path& dst)
    {
        std::error_code ec;
        if(!std::filesystem::is_directory(dst, ec) || ec) {
            spdlog::error("Target is not a directory: {}", dst.string());
            return false;
        }

        auto target = dst / src.filename();
        if(std::filesystem::exists(target, ec) && !ec) {
            xmb->emplace_overlay<app::message_overlay>(
                "Copy failed"_(),
                "A file or folder with that name already exists."_()
            );
            return false;
        }

        if(std::filesystem::is_directory(src, ec) && !ec && target_inside_source(src, target)) {
            xmb->emplace_overlay<app::message_overlay>(
                "Copy failed"_(),
                "A folder cannot be copied into itself."_()
            );
            return false;
        }

        try {
            if(std::filesystem::is_directory(src)) {
                std::filesystem::copy(src, target,
                    std::filesystem::copy_options::recursive |
                    std::filesystem::copy_options::copy_symlinks
                );
            } else {
                std::filesystem::copy_file(src, target);
            }
            return true;
        } catch(const std::exception& e) {
            spdlog::error("Failed to copy {} to {}: {}", src.string(), target.string(), e.what());
            xmb->emplace_overlay<app::message_overlay>(
                "Copy failed"_(),
                std::string{"Failed to copy file: "} + e.what()
            );
            return false;
        }
    }

    bool move_path(app::shell* xmb, const std::filesystem::path& src, const std::filesystem::path& dst)
    {
        std::error_code ec;
        if(!std::filesystem::is_directory(dst, ec) || ec) {
            spdlog::error("Target is not a directory: {}", dst.string());
            return false;
        }

        auto target = dst / src.filename();
        if(std::filesystem::exists(target, ec) && !ec) {
            xmb->emplace_overlay<app::message_overlay>(
                "Move failed"_(),
                "A file or folder with that name already exists."_()
            );
            return false;
        }

        if(std::filesystem::is_directory(src, ec) && !ec && target_inside_source(src, target)) {
            xmb->emplace_overlay<app::message_overlay>(
                "Move failed"_(),
                "A folder cannot be moved into itself."_()
            );
            return false;
        }

        try {
            std::filesystem::rename(src, target);
            return true;
        } catch(const std::exception& e) {
            spdlog::error("Failed to move {} to {}: {}", src.string(), target.string(), e.what());
            xmb->emplace_overlay<app::message_overlay>(
                "Move failed"_(),
                std::string{"Failed to move file: "} + e.what()
            );
            return false;
        }
    }

    bool open_path_external(const std::filesystem::path& file_path)
    {
#if defined(_WIN32)
        return launch_detached({"explorer", file_path.string()});
#elif defined(__APPLE__)
        return launch_detached({"open", file_path.string()});
#else
        if(command_available("xdg-open")) {
            return launch_detached({"xdg-open", file_path.string()});
        }
        return false;
#endif
    }

    } // namespace

    // Implementation of file_info constructor
    file_info::file_info(const std::filesystem::directory_entry& entry) {
        name = entry.path().filename().string();
        display_name = name;
        
        try {
            auto status = entry.status();
            is_directory = std::filesystem::is_directory(status);
            is_symlink = std::filesystem::is_symlink(status);
            is_hidden = !name.empty() && name[0] == '.';
            
            if (std::filesystem::is_regular_file(status)) {
                size = std::filesystem::file_size(entry.path());
            } else {
                size = 0;
            }
            
            modification_time = entry.last_write_time();
            
            // Determine content type based on file extension
            std::string ext = lower_extension(entry.path());
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
    }

    unsigned int files_menu::get_submenus_count() const {
        ensure_built();
        return is_open ? entries.size() : 1;
    }

    menu::menu_entry& files_menu::get_submenu(unsigned int index) const {
        ensure_built();
        return *entries.at(index);
    }

    void files_menu::start_scan_async() {
        // Cancel any in-flight scan by bumping generation
        uint64_t gen = ++scan_generation;
        scanning = true;
        needs_rebuild = false;

        // Show a placeholder while scanning
        entries.clear();
        extra_data_entries.clear();
        {
            dreamrender::texture icon_texture(loader.getDevice(), loader.getAllocator());
            entries.push_back(std::make_unique<action_menu_entry>(
                std::string{"Loading..."}, std::move(icon_texture), std::function<result()>{}
            ));
        }

        // Launch background scan
        std::thread([this, gen, p = path]() {
            std::vector<file_info> file_infos;
            try {
                std::filesystem::directory_iterator it{p};
                for (auto iter = it; iter != std::filesystem::end(it); ++iter) {
                    const auto& entry = *iter;
                    try {
                        if (scan_generation.load() != gen) return; // superseded
                        file_info info(entry);
                        file_infos.push_back(std::move(info));
                    } catch (const std::exception& e) {
                        spdlog::warn("Error processing file {}: {}", entry.path().string(), e.what());
                    }
                }
            } catch (const std::exception& e) {
                spdlog::error("Error scanning directory {}: {}", p.string(), e.what());
            }

            if (scan_generation.load() == gen) {
                {
                    std::lock_guard<std::mutex> lk(cache_mutex);
                    last_scanned_path = p;
                    cached_file_infos.swap(file_infos);
                }
                needs_rebuild = true;
                scanning = false;
            }
        }).detach();
    }

    void files_menu::ensure_built() const {
        if (needs_rebuild.load() && !scanning.load()) {
            needs_rebuild = false;
            const_cast<files_menu*>(this)->rebuild_entries_from_cache();
        }
    }

    void files_menu::stop_scan() {
        ++scan_generation; // supersede any worker
        scanning = false;
    }

    void files_menu::rebuild_entries_from_cache() {
        std::filesystem::path selected_path;
        if(selected_submenu < extra_data_entries.size()) {
            selected_path = extra_data_entries[selected_submenu].path;
        } else {
            selected_path = old_selected_item;
        }

        std::vector<file_info> infos;
        {
            std::lock_guard<std::mutex> lk(cache_mutex);
            infos = cached_file_infos;
        }

        std::vector<file_info> view;
        view.reserve(infos.size());
        for (const auto& info : infos) {
            if (filter(info)) {
                view.push_back(info);
            }
        }

        std::ranges::sort(view, [this](const file_info& a, const file_info& b) {
            bool a_before_b = sort(a, b);
            bool b_before_a = sort(b, a);
            if(a_before_b != b_before_a) {
                return sort_descending ? b_before_a : a_before_b;
            }
            return a.display_name < b.display_name;
        });

        entries.clear();
        extra_data_entries.clear();

        for (const auto& info : view) {
            auto file_path = path / info.name;
            extra_data_entries.push_back({file_path, info});

            auto icon_path = icon_for_file(file_path, info);
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
                    auto fallback = fallback_icon_for(info);
                    if(fallback != icon_path) {
                        try {
                            loader.loadTexture(&entry->get_icon(), fallback);
                        } catch (const std::exception& fallback_error) {
                            spdlog::debug("Failed to load fallback icon for {}: {}", info.name, fallback_error.what());
                        }
                    }
                }
            }

            entries.push_back(std::move(entry));
        }

        selected_submenu = 0;
        if(!selected_path.empty()) {
            if(auto it = std::ranges::find_if(extra_data_entries, [&selected_path](const extra_data& data) {
                return data.path == selected_path;
            }); it != extra_data_entries.end()) {
                selected_submenu = static_cast<unsigned int>(std::distance(extra_data_entries.begin(), it));
            }
        }
    }

    void files_menu::reload() {
        if(selected_submenu < extra_data_entries.size()) {
            old_selected_item = extra_data_entries[selected_submenu].path;
        }

        entries.clear();
        extra_data_entries.clear();

        try {
            std::error_code ec;
            if(!std::filesystem::exists(path, ec) || ec) {
                spdlog::error("Path does not exist: {}", path.string());
                dreamrender::texture icon_texture(loader.getDevice(), loader.getAllocator());
                auto entry = std::make_unique<action_menu_entry>(
                    "Folder not found"_(),
                    std::move(icon_texture),
                    [this]() {
                        xmb->emplace_overlay<app::message_overlay>(
                            "Folder not found"_(),
                            path.string()
                        );
                        return result::success;
                    },
                    std::function<result(action)>{},
                    path.string()
                );
                loader.loadTexture(&entry->get_icon(), config::CONFIG.asset_directory/"icons/icon_files_folder.png");
                entries.push_back(std::move(entry));
                selected_submenu = 0;
                return;
            }

            std::filesystem::path scanned_path;
            {
                std::lock_guard<std::mutex> lk(cache_mutex);
                scanned_path = last_scanned_path;
            }

            // Asynchronous rescan if path changed; otherwise rebuild from cached data
            if (scanned_path != path) {
                start_scan_async();
            } else {
                rebuild_entries_from_cache();
            }
        } catch (const std::exception& e) {
            spdlog::error("Error reloading files menu: {}", e.what());
        }
    }

    result files_menu::open_file(const file_info& info) {
        std::filesystem::path file_path = path / info.name;

        if (info.is_directory) {
            path = file_path;
            reload();
            return result::success;
        }

        auto open_infos = programs::get_open_infos(file_path, programs::file_info(file_path));
        if (!open_infos.empty()) {
            auto& open_info = open_infos[0];
            auto component = open_info.create(file_path, loader);
            if (component) {
                xmb->push_overlay(std::move(component));
                return result::success;
            }
        }

        spdlog::error("No matching program found for file of type \"{}\": {}", info.content_type, file_path.string());
        xmb->emplace_overlay<app::message_overlay>(
            "No matching program found"_(),
            std::string{"No matching program found for file of type \""} + info.content_type + "\": " + file_path.string()
        );
        return result::success;
    }

    void files_menu::show_file_information(const file_info& info, const std::filesystem::path& file_path) {
        std::string message;
        message += "Name: " + info.display_name + "\n";
        message += "Path: " + file_path.string() + "\n";
        message += "Type: " + info.content_type + "\n";
        message += "Size: " + (info.is_directory ? std::string{"Folder"} : format_size(info.size)) + "\n";
        message += "Modified: " + format_file_time(info.modification_time) + "\n";
        message += "Directory: " + std::string(info.is_directory ? "Yes" : "No") + "\n";
        message += "Hidden: " + std::string(info.is_hidden ? "Yes" : "No") + "\n";
        message += "Symbolic link: " + std::string(info.is_symlink ? "Yes" : "No");

        xmb->emplace_overlay<app::message_overlay>(
            "File Information"_(),
            message
        );
    }

    result files_menu::activate_file(const file_info& info, action action) {
        std::filesystem::path file_path = path / info.name;

        if(action == action::ok) {
            return open_file(info);
        }

        if(action == action::options) {
            std::vector<std::string> options;
            std::vector<std::function<void()>> actions;

            options.push_back(info.is_directory ? "Open Folder"_() : "Open"_());
            actions.push_back([this, info]() {
                open_file(info);
            });

            options.push_back("Open using external program"_());
            actions.push_back([this, file_path]() {
                if(!open_path_external(file_path)) {
                    xmb->emplace_overlay<app::message_overlay>(
                        "Cannot Open File"_(),
                        "No external opener is available on this system."_()
                    );
                }
            });

            options.push_back("View information"_());
            actions.push_back([this, info, file_path]() {
                show_file_information(info, file_path);
            });

            options.push_back("Copy"_());
            actions.push_back([this, file_path]() {
                xmb->set_clipboard([xmb = this->xmb, file_path](std::filesystem::path dst) {
                    return copy_path(xmb, file_path, dst);
                });
            });

            options.push_back("Cut"_());
            actions.push_back([this, file_path]() {
                std::weak_ptr<bool> exists = exists_flag;
                auto* shell = xmb;
                xmb->set_clipboard([this, exists, shell, file_path](std::filesystem::path dst) {
                    bool moved = move_path(shell, file_path, dst);
                    if(moved) {
                        if(exists.lock()) {
                            reload();
                        }
                    }
                    return moved;
                });
            });

            options.push_back("Refresh"_());
            actions.push_back([this]() {
                {
                    std::lock_guard<std::mutex> lk(cache_mutex);
                    last_scanned_path.clear();
                }
                reload();
            });

            if(const auto& cb = xmb->get_clipboard()) {
                if(std::holds_alternative<std::function<bool(std::filesystem::path)>>(*cb)) {
                    options.push_back("Paste here"_());
                    actions.push_back([this]() {
                        if(const auto& clipboard = xmb->get_clipboard()) {
                            if(auto f = std::get_if<std::function<bool(std::filesystem::path)>>(&clipboard.value())) {
                                if((*f)(path)) {
                                    {
                                        std::lock_guard<std::mutex> lk(cache_mutex);
                                        last_scanned_path.clear();
                                    }
                                    reload();
                                }
                            }
                        }
                    });

                    if(info.is_directory) {
                        options.push_back("Paste into this folder"_());
                        actions.push_back([this, file_path]() {
                            if(const auto& clipboard = xmb->get_clipboard()) {
                                if(auto f = std::get_if<std::function<bool(std::filesystem::path)>>(&clipboard.value())) {
                                    if((*f)(file_path)) {
                                        {
                                            std::lock_guard<std::mutex> lk(cache_mutex);
                                            last_scanned_path.clear();
                                        }
                                        reload();
                                    }
                                }
                            }
                        });
                    }
                }
            }

            xmb->emplace_overlay<app::choice_overlay>(options, 0, [actions = std::move(actions)](unsigned int index) {
                if(index < actions.size()) {
                    actions[index]();
                }
            });
            return result::success;
        }

        return result::unsupported;
    }

    void files_menu::show_sort_filter_options() {
        std::vector<std::string> options{
            std::string{"Filter: "} + std::string(filters[selected_filter].first),
            std::string{"Sort: "} + std::string(sorts[selected_sort].first),
            std::string{"Order: "} + (sort_descending ? "Descending"_() : "Ascending"_()),
            "Refresh"_()
        };

        xmb->emplace_overlay<app::choice_overlay>(options, 0, [this](unsigned int index) {
            switch(index) {
                case 0:
                    selected_filter = (selected_filter + 1) % filters.size();
                    filter = filters[selected_filter].second;
                    resort();
                    break;
                case 1:
                    selected_sort = (selected_sort + 1) % sorts.size();
                    sort = sorts[selected_sort].second;
                    resort();
                    break;
                case 2:
                    sort_descending = !sort_descending;
                    resort();
                    break;
                case 3:
                    {
                        std::lock_guard<std::mutex> lk(cache_mutex);
                        last_scanned_path.clear();
                    }
                    reload();
                    break;
                default:
                    break;
            }
        });
    }

    result files_menu::activate(action action) {
        if(action == action::extra) {
            show_sort_filter_options();
            return result::success;
        }

        auto r = simple_menu::activate(action);
        if(r != result::unsupported) {
            return r;
        }

        if(action == action::options) {
            show_sort_filter_options();
            return result::success;
        }

        return result::unsupported;
    }

    void files_menu::get_button_actions(std::vector<std::pair<action, std::string>>& v) {
        if(!v.empty()) {
            return;
        }
        v.emplace_back(action::none, "");
        v.emplace_back(action::none, "");
        v.emplace_back(action::options, "Options"_());
        v.emplace_back(action::extra, "Sort and Filter"_());
    }

    void files_menu::on_open() {
        simple_menu::on_open();
        reload();
    }

    void files_menu::resort() {
        try {
            rebuild_entries_from_cache();
        } catch(const std::exception& e) {
            spdlog::error("Error resorting files menu: {}", e.what());
        }
    }

}
