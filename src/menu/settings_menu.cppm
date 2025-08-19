module;

#include <string>

export module xmbshell.app:settings_menu;

import spdlog;
import glibmm;
import giomm;
import dreamrender;
import xmbshell.config;
import :menu_base;
import :menu_utils;

namespace app {
    class xmbshell;
}

export namespace menu {

class settings_menu : public simple_menu {
    public:
        settings_menu(std::string name, dreamrender::texture&& icon, app::xmbshell* xmb, dreamrender::resource_loader& loader);
        ~settings_menu() override = default;
};

}
