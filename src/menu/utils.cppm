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
