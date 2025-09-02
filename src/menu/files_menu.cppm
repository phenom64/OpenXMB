/* XMBShell, a console-like desktop shell
 * Copyright (C) 2025 - JCM
 *
 * This file (or substantial portions of it) is derived from XMBShell:
 *   https://github.com/JnCrMx/xmbshell
 *
 * Modified by Syndromatic Ltd for OpenXMB.
 * Portions Copyright (C) 2025 Syndromatic Ltd, Kavish Krishnakumar.
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

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

export module openxmb.app:files_menu;

import spdlog;
import dreamrender;
import openxmb.config;
import openxmb.utils;
import :menu_base;
import :menu_utils;

namespace app {
    class shell;
}

export namespace menu {

// JSON-based file info structure to replace Gio::FileInfo
struct file_info {
    std::string name;
    std::string display_name;
    std::string content_type;
    std::uint64_t size;
    bool is_directory;
    bool is_hidden;
    bool is_symlink;
    std::filesystem::file_time_type modification_time;
    
    file_info() = default;
    file_info(const std::filesystem::directory_entry& entry);
};

class files_menu : public simple_menu {
    public:
        files_menu(std::string name, dreamrender::texture&& icon, app::shell* xmb, std::filesystem::path path, dreamrender::resource_loader& loader);
        ~files_menu() override = default;

        void on_open() override;
        void on_close() override {
            simple_menu::on_close();
            if(selected_submenu < extra_data_entries.size()) {
                old_selected_item = extra_data_entries[selected_submenu].path;
            }
            entries.clear();
            extra_data_entries.clear();
        }

        unsigned int get_submenus_count() const override {
            return is_open ? entries.size() : 1;
        }
        result activate(action action) override;

        void get_button_actions(std::vector<std::pair<action, std::string>>& v) override;

        static constexpr auto filter_all = [](const file_info&) { return true; };
        static constexpr auto filter_visible = [](const file_info& info) {
            return !info.is_hidden;
        };

        static constexpr auto sort_by_name = [](const file_info& a, const file_info& b) {
            return a.display_name.compare(b.display_name) < 0;
        };
        static constexpr auto sort_by_size = [](const file_info& a, const file_info& b) {
            return a.size < b.size;
        };
        static constexpr auto sort_by_type = [](const file_info& a, const file_info& b) {
            return a.content_type.compare(b.content_type) < 0;
        };

        using filter_entry_type = std::pair<std::string_view, std::add_pointer_t<bool(const file_info&)>>;
        static constexpr std::array filters{
            filter_entry_type{"Normal", filter_visible},
            filter_entry_type{"All files", filter_all},
        };

        using sort_entry_type = std::pair<std::string_view, std::add_pointer_t<bool(const file_info& a, const file_info& b)>>;
        static constexpr std::array sorts{
            sort_entry_type{"Name", sort_by_name},
            sort_entry_type{"Size", sort_by_size},
            sort_entry_type{"Type", sort_by_type},
        };
    private:
        void reload();
        void resort();
        result activate_file(const file_info& info, action action);

        app::shell* xmb;
        std::filesystem::path path;
        dreamrender::resource_loader& loader;

        struct extra_data {
            std::filesystem::path path;
            file_info info;
        };
        std::vector<extra_data> extra_data_entries;

        std::function<bool(const file_info&)> filter = filter_visible;
        std::function<bool(const file_info& a, const file_info& b)> sort = sort_by_name;

        int selected_filter = 0;
        int selected_sort = 0;
        bool sort_descending = false;

        std::filesystem::path old_selected_item;
        // This is extremely hacky, but it works for now.
        std::shared_ptr<bool> exists_flag = std::make_shared<bool>(true);
};

}
