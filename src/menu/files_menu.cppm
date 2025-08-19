module;

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

export module xmbshell.app:files_menu;

import spdlog;
import glibmm;
import giomm;
import dreamrender;
import xmbshell.config;
import xmbshell.utils;
import :menu_base;
import :menu_utils;

namespace app {
    class xmbshell;
}

export namespace menu {

class files_menu : public simple_menu {
    public:
        files_menu(std::string name, dreamrender::texture&& icon, app::xmbshell* xmb, std::filesystem::path path, dreamrender::resource_loader& loader);
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

        static constexpr auto filter_all = [](const Gio::FileInfo&) { return true; };
        static constexpr auto filter_visible = [](const Gio::FileInfo& info) {
            return !info.is_hidden() && !info.is_hidden();
        };

        static constexpr auto sort_by_name = [](const Gio::FileInfo& a, const Gio::FileInfo& b) {
            return a.get_display_name().compare(b.get_display_name()) < 0;
        };
        static constexpr auto sort_by_size = [](const Gio::FileInfo& a, const Gio::FileInfo& b) {
            return a.get_size() < b.get_size();
        };
        static constexpr auto sort_by_type = [](const Gio::FileInfo& a, const Gio::FileInfo& b) {
            auto av = a.get_attribute_string("standard::fast-content-type");
            auto bv = b.get_attribute_string("standard::fast-content-type");
            return av.compare(bv) < 0;
        };

        using filter_entry_type = std::pair<std::string_view, std::add_pointer_t<bool(const Gio::FileInfo&)>>;
        static constexpr std::array filters{
            filter_entry_type{"Normal", filter_visible},
            filter_entry_type{"All files", filter_all},
        };

        using sort_entry_type = std::pair<std::string_view, std::add_pointer_t<bool(const Gio::FileInfo&, const Gio::FileInfo&)>>;
        static constexpr std::array sorts{
            sort_entry_type{"Name", sort_by_name},
            sort_entry_type{"Size", sort_by_size},
            sort_entry_type{"Type", sort_by_type},
        };
    private:
        void reload();
        void resort();

        app::xmbshell* xmb;
        std::filesystem::path path;
        dreamrender::resource_loader& loader;

        struct extra_data {
            std::filesystem::path path;
            Glib::RefPtr<Gio::File> file;
            Glib::RefPtr<Gio::FileInfo> info;
        };
        std::vector<extra_data> extra_data_entries;

        std::function<bool(const Gio::FileInfo&)> filter = filter_visible;
        std::function<bool(const Gio::FileInfo& a, const Gio::FileInfo& b)> sort = sort_by_name;

        int selected_filter = 0;
        int selected_sort = 0;
        bool sort_descending = false;

        std::filesystem::path old_selected_item;
        // This is extremely hacky, but it works for now.
        std::shared_ptr<bool> exists_flag = std::make_shared<bool>(true);
        friend bool cut_file(app::xmbshell* xmb, std::weak_ptr<void> exists, files_menu* ptr, const std::filesystem::path& src, const std::filesystem::path& dst);
};

}
