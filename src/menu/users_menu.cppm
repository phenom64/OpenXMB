/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025 Syndromatic Ltd. All rights reserved
 * Designed by Kavish Krishnakumar in Manchester.
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

#include <string>
#include <vector>

export module openxmb.app:users_menu;

import :menu_base;
import dreamrender;

namespace app {
    class shell;
}

export namespace menu {

// JSON-based user info structure
struct user_info {
    std::string username;
    std::string real_name;
    std::string home_directory;
    std::string shell;
    bool is_active;
    bool is_admin;
    
    user_info() = default;
    user_info(const std::string& name);
};

class users_menu : public simple_menu {
    public:
        users_menu(std::string name, dreamrender::texture&& icon, app::shell* xmb, dreamrender::resource_loader& loader);
        ~users_menu() override = default;
        
        result activate(action action) override;
        void get_button_actions(std::vector<std::pair<action, std::string>>& v) override;
    private:
        void reload();
        std::vector<user_info> scan_users();
        result activate_user(const user_info& user, action action);
        
        app::shell* xmb;
        dreamrender::resource_loader& loader;
        std::vector<user_info> users;
};

}
