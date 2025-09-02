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

#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

export module openxmb.app:programs;

import :component;

namespace programs {

export struct open_info {
    std::string name;
    bool is_external;
    std::function<std::unique_ptr<app::component>(std::filesystem::path, dreamrender::resource_loader&)> create;
};

// JSON-based file info structure to replace Gio::FileInfo
export struct file_info {
    std::string mime_type;
    std::string content_type;
    std::string name;
    std::string display_name;
    std::string icon_name;
    std::string content_type_string;
    std::string fast_content_type;
    
    file_info() = default;
    file_info(const std::filesystem::path& path);
};

class program_registry {
    static std::unordered_multimap<std::string, open_info> programs_by_mimetype;
    static std::unordered_multimap<std::string, open_info> programs_by_file_extension;

    protected:
        friend std::vector<open_info> get_open_infos(const std::filesystem::path& path, const file_info& info);

        void do_register_program_mime(std::string name, std::string mime_type, open_info info) {
            programs_by_mimetype.emplace(mime_type, info);
        }
        void do_register_program_ext(std::string name, std::string ext, open_info info) {
            programs_by_file_extension.emplace(ext, info);
        }

        static void get_program_mime(std::string mime_type, std::output_iterator<open_info> auto out) {
            auto range = programs_by_mimetype.equal_range(mime_type);
            for(auto it = range.first; it != range.second; ++it) {
                *out++ = it->second;
            }
        }
        static void get_program_ext(std::string ext, std::output_iterator<open_info> auto out) {
            auto range = programs_by_file_extension.equal_range(ext);
            for(auto it = range.first; it != range.second; ++it) {
                *out++ = it->second;
            }
        }
};
std::unordered_multimap<std::string, open_info> program_registry::programs_by_mimetype{};
std::unordered_multimap<std::string, open_info> program_registry::programs_by_file_extension{};

export template<typename T>
struct register_program : program_registry {
    register_program(std::string name, std::initializer_list<std::string> mime_types,
                     std::initializer_list<std::string> file_extensions) {
        for(auto& mime_type : mime_types) {
            do_register_program_mime(name, mime_type, {name, false, [](std::filesystem::path path, dreamrender::resource_loader& loader) {
                return std::make_unique<T>(path, loader);
            }});
        }
        for(auto& ext : file_extensions) {
            do_register_program_ext(name, ext, {name, false, [](std::filesystem::path path, dreamrender::resource_loader& loader) {
                return std::make_unique<T>(path, loader);
            }});
        }
    }
};

export std::vector<open_info> get_open_infos(const std::filesystem::path& path, const file_info& info) {
    std::vector<open_info> infos;

    program_registry::get_program_mime(info.fast_content_type, std::back_inserter(infos));
    program_registry::get_program_ext(path.extension().string(), std::back_inserter(infos));

    return infos;
}

}
