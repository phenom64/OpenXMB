module;

#include <algorithm>
#include <filesystem>
#include <functional>
#include <numeric>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

#include <unistd.h>

module xmbshell.app;

import :files_menu;
import :menu_base;
import :menu_utils;
import :message_overlay;
import :choice_overlay;
import :programs;

import xmbshell.config;
import xmbshell.utils;
import dreamrender;
import glibmm;
import giomm;
import sdl2;
import spdlog;
import i18n;

namespace menu {
    using namespace mfk::i18n::literals;

    files_menu::files_menu(std::string name, dreamrender::texture&& icon, app::xmbshell* xmb, std::filesystem::path path, dreamrender::resource_loader& loader)
    : simple_menu(std::move(name), std::move(icon)), xmb(xmb), path(std::move(path)), loader(loader)
    {

    }

    void files_menu::reload() {
        if(selected_submenu < extra_data_entries.size()) {
            old_selected_item = extra_data_entries[selected_submenu].path;
        }

        std::filesystem::directory_iterator it{path};

        std::unordered_set<std::filesystem::path> all_files;
        for (const auto& entry : it) {
            try {
                auto file = Gio::File::create_for_path(entry.path().string());
                auto info = file->query_info(
                    "standard::type"                ","
                    "standard::fast-content-type"   ","
                    "standard::display-name"        ","
                    "standard::symbolic-icon"       ","
                    "standard::icon"                ","
                    "standard::is-hidden"           ","
                    "standard::is-backup"           ","
                    "thumbnail::path"               ","
                    "thumbnail::is-valid");
                if(!info) {
                    spdlog::warn("Failed to query info for entry: {}", entry.path().string());
                    continue;
                }

                std::string display_name = info->get_display_name();
                std::string content_type = info->get_attribute_string("standard::fast-content-type");
                bool thumbnail_is_valid = info->get_attribute_boolean("thumbnail::is-valid");
                std::string thumbnail_path = info->get_attribute_as_string("thumbnail::path");
                std::string extension = entry.path().extension().string();

                if(!filter(*info.get())) {
                    continue;
                }

                // Skip entries we already have (TODO: update icon if needed)
                if(auto it = std::ranges::find_if(extra_data_entries, [&entry](const auto& e) {
                    return e.path == entry.path();
                }); it != extra_data_entries.end()) {
                    it->file = file;
                    it->info = info;
                    all_files.insert(entry.path());
                    continue;
                }

                std::string content_type_key = content_type;
                std::ranges::replace(content_type_key, '/', '_');

                std::filesystem::path icon_file_path = config::CONFIG.asset_directory/
                    "icons"/("icon_files_type_"+content_type_key+".png");
                if(thumbnail_is_valid && !thumbnail_path.empty()) {
                    icon_file_path = thumbnail_path;
                } else if(content_type.starts_with("image/") || extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp") {
                    icon_file_path = entry.path(); // This might be incredibly inefficient, but it will work for now
                } else if(std::filesystem::exists(icon_file_path)) {
                    // do nothing here, we already have a specialized icon for this file type
                } else if(auto r = utils::resolve_icon(info->get_symbolic_icon().get())) {
                    icon_file_path = *r;
                } else if(auto r = utils::resolve_icon(info->get_icon().get())) {
                    icon_file_path = *r;
                }

                if(!std::filesystem::exists(icon_file_path)) {
                    spdlog::warn("Icon file does not exist: {}", icon_file_path.string());
                    icon_file_path = config::CONFIG.asset_directory/
                        "icons"/(entry.is_directory() ? "icon_files_folder.png" : "icon_files_file.png");
                }

                if(entry.is_directory()) {
                    auto menu = make_simple<files_menu>(entry.path().filename().string(), icon_file_path, loader, xmb, entry.path(), loader);
                    entries.push_back(std::move(menu));
                }
                else if (entry.is_regular_file()) {
                    auto menu = make_simple<simple_menu_entry>(entry.path().filename().string(), icon_file_path, loader);
                    entries.push_back(std::move(menu));
                } else {
                    spdlog::warn("Unsupported file type: {}", entry.path().string());
                    continue;
                }
                extra_data_entries.emplace_back(entry.path(), file, info);
                all_files.insert(entry.path());
            } catch(const std::exception& e) {
                spdlog::error("Failed to process entry: {}: {}", entry.path().string(), e.what());
            }
        }

        for(auto it = entries.begin(); it != entries.end();) {
            auto dist = std::distance(entries.begin(), it);
            if(!all_files.contains(extra_data_entries[dist].path)) {
                it = entries.erase(it);
                extra_data_entries.erase(extra_data_entries.begin() + dist);

                if(selected_submenu == dist) {
                    selected_submenu = 0;
                } else if(selected_submenu > dist) {
                    --selected_submenu;
                }
            } else {
                ++it;
            }
        }

        bool to_beginning = false;
        if(auto it = std::ranges::find_if(extra_data_entries, [this](const auto& e) {
            return e.path == old_selected_item;
        }); it != extra_data_entries.end()) {
            selected_submenu = std::distance(extra_data_entries.begin(), it);
        } else {
            selected_submenu = 0;
            to_beginning = true;
        }
        resort();
        if(to_beginning) {
            selected_submenu = 0;
        }
    }

    void files_menu::resort() {
        if(selected_submenu < extra_data_entries.size()) {
            old_selected_item = extra_data_entries[selected_submenu].path;
        }

        std::vector<unsigned int> indices(entries.size());
        std::iota(indices.begin(), indices.end(), 0);

        std::ranges::sort(indices, [this](unsigned int a, unsigned int b) {
            const auto& a_entry = *extra_data_entries[a].info.get();
            const auto& b_entry = *extra_data_entries[b].info.get();
            return sort(a_entry, b_entry) ^ sort_descending; // Cursed XOR usage
        });

        // This is not very efficient, but who cares.
        decltype(entries) old_entries = std::move(entries);
        decltype(extra_data_entries) old_extra_data_entries = std::move(extra_data_entries);
        entries.clear();
        extra_data_entries.clear();
        for(auto i : indices) {
            entries.push_back(std::move(old_entries[i]));
            extra_data_entries.push_back(std::move(old_extra_data_entries[i]));
        }

        if(auto it = std::ranges::find_if(extra_data_entries, [this](const auto& e) {
            return e.path == old_selected_item;
        }); it != extra_data_entries.end()) {
            selected_submenu = std::distance(extra_data_entries.begin(), it);
        } else {
            selected_submenu = 0;
        }
    }

    void files_menu::on_open() {
        simple_menu::on_open();

        if(!std::filesystem::exists(path)) {
            spdlog::error("Path does not exist: {}", path.string());
            return;
        }

        reload();
    }

    bool copy_file(app::xmbshell* xmb, const std::filesystem::path& src, const std::filesystem::path& dst);
    bool cut_file(app::xmbshell* xmb, std::weak_ptr<void> exists, files_menu* ptr, const std::filesystem::path& src, const std::filesystem::path& dst);

    result files_menu::activate(action action)
    {
        auto r = simple_menu::activate(action);
        if(r != result::unsupported) {
            if(action == action::ok || r != result::submenu || !is_open) {
                return r;
            }
        }

        if(selected_submenu >= extra_data_entries.size()) {
            return result::failure;
        }
        auto& [path, file, info] = extra_data_entries.at(selected_submenu);

        auto action_open = [this, path, info](){
            auto open_infos = programs::get_open_infos(path, *info.get());
            if(open_infos.empty()) {
                std::string p = path.string();
                std::string mime_type = info->get_attribute_string("standard::fast-content-type");
                spdlog::error("No machting program found for file of type \"{}\": {}", mime_type, p);
                xmb->emplace_overlay<app::message_overlay>("No machting program found"_(),
                    "No matching program found for file of type \"{}\": {}"_(mime_type, p),
                    std::vector<std::string>{"OK"_()});
                return;
            }
            programs::open_info open_info = open_infos.front();
            xmb->push_overlay(open_info.create(path, loader));
        };
        if(action == action::ok) {
            if(info->get_file_type() == Gio::FileType::DIRECTORY) {
                return result::unsupported;
            }
            action_open();
            return result::success;
        }
        else if(action == action::options) {
            auto action_open_external = [](){};
            auto action_view_information = [](){};
            auto action_copy = [this, path](){
                xmb->set_clipboard([xmb = this->xmb, path](std::filesystem::path dst){
                    return copy_file(xmb, path, dst);
                });
            };
            auto action_cut = [this, path](){
                xmb->set_clipboard([xmb = this->xmb, exists = std::weak_ptr<bool>{exists_flag}, ptr = this, path](std::filesystem::path dst){
                    return cut_file(xmb, exists, ptr, path, dst);
                });
            };
            auto action_delete = [](){};
            auto action_refresh = [this](){
                reload();
            };
            auto action_paste_here = [this, path = this->path](){
                if(const auto& cb = xmb->get_clipboard()) {
                    if(auto f = std::get_if<std::function<bool(std::filesystem::path)>>(&cb.value())) {
                        if((*f)(path)) {
                            reload();
                        }
                    }
                }
            };
            auto action_paste_into = [this, path](){
                if(const auto& cb = xmb->get_clipboard()) {
                    if(auto f = std::get_if<std::function<bool(std::filesystem::path)>>(&cb.value())) {
                        if((*f)(path)) {
                            reload();
                        }
                    }
                }
            };

            // If anyone messes up the order, we might rm -rf the user's home... oh well.
            std::vector<std::function<void()>> actions = {
                action_open, action_open_external, action_view_information, action_copy, action_cut, action_delete, action_refresh
            };
            std::vector options{
                "Open"_(),   "Open using external program"_(), "View information"_(),   "Copy"_(),   "Cut"_(),   "Delete"_(),   "Refresh"_()
            };
            if(const auto& cb = xmb->get_clipboard()) {
                if(std::holds_alternative<std::function<bool(std::filesystem::path)>>(*cb)) {
                    options.push_back("Paste here"_()); actions.emplace_back(action_paste_here);
                    if(info->get_file_type() == Gio::FileType::DIRECTORY) {
                        options.push_back("Paste into this folder"_()); actions.emplace_back(action_paste_into);
                    }
                }
            }
            xmb->emplace_overlay<app::choice_overlay>(options, 0, [this, actions](unsigned int index){
                actions.at(index)();
            });
            return result::success;
        }
        return result::unsupported;
    }

    bool copy_file(app::xmbshell* xmb, const std::filesystem::path& src, const std::filesystem::path& dst) {
        std::filesystem::path p = dst;
        if(!std::filesystem::is_directory(p)) {
            spdlog::error("Target is not a directory: {}", p.string());
            return false;
        }
        p /= src.filename();

        if(std::filesystem::exists(p)) {
            spdlog::error("File already exists: {}", p.string());
            return false;
        }

        try {
            std::filesystem::copy_file(src, p, std::filesystem::copy_options::overwrite_existing);
            return true;
        } catch(const std::exception& e) {
            spdlog::error("Failed to copy file: {}", e.what());

            std::string message = e.what();
            xmb->emplace_overlay<app::message_overlay>("Copy failed"_(), "Failed to copy file: {}"_(message),
                std::vector<std::string>{"OK"_()}
            );
        }
        return false;
    }

    bool cut_file(app::xmbshell* xmb, std::weak_ptr<void> exists, files_menu* ptr, const std::filesystem::path& src, const std::filesystem::path& dst) {
        std::filesystem::path p = dst;
        if(!std::filesystem::is_directory(p)) {
            spdlog::error("Target is not a directory: {}", p.string());
            return false;
        }
        p /= src.filename();

        if(std::filesystem::exists(p)) {
            spdlog::error("File already exists: {}", p.string());
            return false;
        }

        try {
            std::filesystem::rename(src, p);
            if(auto sp = exists.lock()) {
                spdlog::debug("Reloading source after move operation");
                ptr->reload();
            }
            return true;
        } catch(const std::exception& e) {
            spdlog::error("Failed to copy file: {}", e.what());

            std::string message = e.what();
            xmb->emplace_overlay<app::message_overlay>("Move failed"_(), "Failed to move file: {}"_(message),
                std::vector<std::string>{"OK"_()}
            );
        }
        return false;
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
}
