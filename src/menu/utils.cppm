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

#include <array>
#include <filesystem>
#include <memory>
#include <string>

export module openxmb.app:menu_utils;
import dreamrender;
import :menu_base;

export namespace menu {

template<typename Menu, typename... Args>
std::unique_ptr<Menu> make_simple(std::string name, std::filesystem::path icon_path,
    dreamrender::resource_loader& loader,
    Args&&... args)
{
    auto menu = std::make_unique<Menu>(std::move(name), dreamrender::texture(loader.getDevice(), loader.getAllocator()), std::forward<Args>(args)...);
    loader.loadTexture(&menu->get_icon(), std::move(icon_path));
    return menu;
}

template<typename Menu, typename... Args>
std::unique_ptr<simple<Menu>> make_simple_of(std::string name, std::filesystem::path icon_path,
    dreamrender::resource_loader& loader,
    Args&&... args)
{
    return make_simple<simple<Menu>>(std::move(name), std::move(icon_path), loader, std::forward<Args>(args)...);
}

}
