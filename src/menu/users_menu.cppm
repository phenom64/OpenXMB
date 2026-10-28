/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025-2026 Syndromatic Ltd. All rights reserved
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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

export module openxmb.app:users_menu;

import :menu_base;
import auroreengine;

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
    std::uint64_t uid;
    std::uint64_t gid;
    bool is_active;
    bool is_admin;
    
    user_info() = default;
    user_info(const std::string& name);
};

class users_menu : public simple_menu {
    public:
        users_menu(std::string name, auroreengine::texture&& icon, app::shell* xmb, auroreengine::resource_loader& loader);
        ~users_menu() override = default;
        
        // Extra OpenXMB rows listed below the PS3 user rows; reloads keep them.
        void append_rows(std::vector<std::unique_ptr<menu_entry>> rows);
        // The signed-in user's row carries the diamond marker.
        [[nodiscard]] bool is_signed_in_user(unsigned int index) const { return core_rows > 0 && index == core_rows - 1; }
        result activate(action action) override;
        void get_button_actions(std::vector<std::pair<action, std::string>>& v) override;
    private:
        void reload();
        std::vector<user_info> scan_users();
        result activate_user(const user_info& user, action action);
        
        app::shell* xmb;
        auroreengine::resource_loader& loader;
        std::vector<user_info> users;
        std::size_t core_rows = 0;
};

}
