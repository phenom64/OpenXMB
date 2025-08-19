module;

#include <string>

export module xmbshell.app:users_menu;

import :menu_base;

import dreamrender;
import glibmm;
import giomm;

namespace app {
    class xmbshell;
}

export namespace menu {

class users_menu : public simple_menu {
    public:
        users_menu(std::string name, dreamrender::texture&& icon, app::xmbshell* xmb, dreamrender::resource_loader& loader);
        ~users_menu() override = default;
    private:
        Glib::RefPtr<Gio::DBus::Proxy> login1, accounts;
};

}
