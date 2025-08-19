module;

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

export module xmbshell.app:programs;

import :component;
import giomm;

namespace programs {

export struct open_info {
    std::string name;
    bool is_external;
    std::function<std::unique_ptr<app::component>(std::filesystem::path, dreamrender::resource_loader&)> create;
};

class program_registry {
    static std::unordered_multimap<std::string, open_info> programs_by_mimetype;
    static std::unordered_multimap<std::string, open_info> programs_by_file_extension;

    protected:
        friend std::vector<open_info> get_open_infos(const std::filesystem::path& path, const Gio::FileInfo& info);

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

export std::vector<open_info> get_open_infos(const std::filesystem::path& path, const Gio::FileInfo& info) {
    std::vector<open_info> infos;

    program_registry::get_program_mime(info.get_attribute_string("standard::fast-content-type"), std::back_inserter(infos));
    program_registry::get_program_ext(path.extension().string(), std::back_inserter(infos));

    return infos;
}

}
