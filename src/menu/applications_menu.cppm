module;

#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <filesystem>
#include <vector>

export module openxmb.app:applications_menu;
import :menu_base;
import dreamrender;
import spdlog;

namespace app {
    class shell;
}

export namespace menu {

// JSON-based application info structure
struct app_info {
    std::string id;
    std::string name;
    std::string comment;
    std::string exec;
    std::string icon;
    std::string categories;
    bool terminal;
    bool hidden;
    
    app_info() = default;
    app_info(const std::filesystem::path& desktop_file);
};

using AppFilter = std::function<bool(const app_info&)>;

constexpr auto andFilter(AppFilter a, AppFilter b) {
    return [a, b](const app_info& app) {
        return a(app) && b(app);
    };
}

constexpr auto noFilter() {
    return [](const app_info&) {
        return true;
    };
}

constexpr auto categoryFilter(const std::string& category) {
    return [category](const app_info& app) {
        std::istringstream iss(app.categories);
        std::string c;
        while(std::getline(iss, c, ';')) {
            if(c == category) {
                return true;
            }
        }
        return false;
    };
}

constexpr auto excludeFilter(const std::unordered_set<std::string>& ids) {
    return [&ids](const app_info& app) {
        return !ids.contains(app.id);
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
        std::unique_ptr<action_menu_entry> create_action_menu_entry(const app_info& app, bool hidden = false);
        result activate_app(const app_info& app, action action);
        std::vector<app_info> scan_applications();

        app::shell* xmb;
        dreamrender::resource_loader& loader;

        bool show_hidden = false;
        AppFilter filter;
        std::vector<app_info> apps;
};

}
