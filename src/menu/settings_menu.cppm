module;

#include <string>

export module openxmb.app:settings_menu;

import spdlog;
import dreamrender;
import openxmb.config;
import :menu_base;
import :menu_utils;

namespace app {
    class shell;
}

export namespace menu {

class settings_menu : public simple_menu {
    public:
        settings_menu(std::string name, dreamrender::texture&& icon, app::shell* xmb, dreamrender::resource_loader& loader);
        ~settings_menu() override = default;
};

}