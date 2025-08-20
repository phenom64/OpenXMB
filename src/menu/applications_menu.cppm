module;

#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>

export module shell.app:applications_menu;
import :menu_base;
import dreamrender;
import glibmm;
import giomm;
import spdlog;

namespace app {
    class shell;
}

export namespace menu {
    using AppFilter = std::function<bool(Gio::AppInfo*)>;

    constexpr auto andFilter(AppFilter a, AppFilter b) {
        return [a, b](Gio::AppInfo* app) {
            return a(app) && b(app);
        };
    }
    constexpr auto noFilter() {
        return [](Gio::AppInfo*) {
            return true;
        };
    }
    constexpr auto categoryFilter(const std::string& category) {
        return [category](Gio::AppInfo* app) {
#if __linux__
            auto desktop_app = dynamic_cast<Gio::DesktopAppInfo*>(app);
            if(!desktop_app) {
                return false;
            }

            std::istringstream iss(desktop_app->get_categories());
            std::string c;
            while(std::getline(iss, c, ';')) {
                if(c == category) {
                    return true;
                }
            }
#endif
            return false;
        };
    }
    constexpr auto excludeFilter(const std::unordered_set<std::string>& ids) {
        return [&ids](Gio::AppInfo*app) {
            return !ids.contains(app->get_id());
        };
    }

    class applications_menu : public simple_menu {
        public:
            applications_menu(std::string name, dreamrender::texture&& icon, app::shell* xmb, dreamrender::resource_loader& loader, AppFilter filter = noFilter());
            ~applications_menu() override = default;

            result activate(action action) override;
            void get_button_actions(std::vector<std::pair<action, std::string>>& v) override;
        private:
            void reload();
            std::unique_ptr<action_menu_entry> create_action_menu_entry(Glib::RefPtr<Gio::AppInfo> app, bool hidden = false);
            result activate_app(Glib::RefPtr<Gio::AppInfo> app, action action);

            app::shell* xmb;
            dreamrender::resource_loader& loader;

            bool show_hidden = false;
            AppFilter filter;
            std::vector<Glib::RefPtr<Gio::AppInfo>> apps;
    };

}