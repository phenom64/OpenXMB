module;

#include <chrono>

module openxmb.app;

import sdl2;
import spdlog;
import i18n;
import glm;
import vulkan_hpp;
import vma;
import dreamrender;

import openxmb.config;
import :menu_base;
import :menu_utils;
import :applications_menu;
import :settings_menu;
import :users_menu;
import :files_menu;

using namespace mfk::i18n::literals;

namespace app {

main_menu::main_menu(app::shell* xmb) : xmb(xmb) {

}

void main_menu::preload(vk::Device device, vma::Allocator allocator, dreamrender::resource_loader& loader) {
    using ::menu::make_simple;
    using ::menu::make_simple_of;

    const auto& asset_directory = config::CONFIG.asset_directory;
    menus.push_back(make_simple<menu::users_menu>("Users"_(), asset_directory/"icons/icon_category_users.png", loader, xmb, loader));
    menus.push_back(make_simple<menu::settings_menu>("Settings"_(), asset_directory/"icons/icon_category_settings.png", loader, xmb, loader));
    menus.push_back(make_simple<menu::files_menu>("Photo"_(), asset_directory/"icons/icon_category_photo.png", loader, xmb,
        config::CONFIG.picturesPath, loader));
    menus.push_back(make_simple<menu::files_menu>("Music"_(), asset_directory/"icons/icon_category_music.png", loader, xmb,
        config::CONFIG.musicPath, loader));
    menus.push_back(make_simple<menu::files_menu>("Video"_(), asset_directory/"icons/icon_category_video.png", loader, xmb,
        config::CONFIG.videosPath, loader));
    menus.push_back(make_simple_of<menu::menu>("TV"_(), asset_directory/"icons/icon_category_tv.png", loader));
    menus.push_back(make_simple<menu::applications_menu>("Game"_(), asset_directory/"icons/icon_category_game.png", loader, xmb, loader, ::menu::categoryFilter("Game")));
    menus.push_back(make_simple<menu::applications_menu>("Application"_(), asset_directory/"icons/icon_category_application.png", loader, xmb, loader));
    menus.push_back(make_simple_of<menu::menu>("Network"_(), asset_directory/"icons/icon_category_network.png", loader));
    menus.push_back(make_simple_of<menu::menu>("Friends"_(), asset_directory/"icons/icon_category_friends.png", loader));

    menus[selected]->on_open();
}

result main_menu::on_action(action action) {
    switch(action) {
        case action::left:
            return select_relative(direction::left) ? result::success | result::ok_sound : result::unsupported | result::error_rumble;
        case action::right:
            return select_relative(direction::right) ? result::success | result::ok_sound : result::unsupported | result::error_rumble;
        case action::up:
            return select_relative(direction::up) ? result::success | result::ok_sound : result::unsupported | result::error_rumble;
        case action::down:
            return select_relative(direction::down) ? result::success | result::ok_sound : result::unsupported | result::error_rumble;
        case action::ok:
        case action::options:
        case action::extra:
            return activate_current(action) ? result::success : result::unsupported | result::error_rumble;
        case action::cancel:
            return back() ? result::success : result::unsupported | result::error_rumble;
        default:
            return result::unsupported;
    }
    return result::unsupported;
}

bool main_menu::select_relative(direction dir) {
    if(!in_submenu) {
        if(dir == direction::left) {
            if(selected > 0) {
                select(selected-1);
                return true;
            }
        } else if(dir == direction::right) {
            if(selected < menus.size()-1) {
                select(selected+1);
                return true;
            }
        } else if(dir == direction::up) {
            auto& menu = menus[selected];
            if(menu->get_selected_submenu() > 0) {
                select_menu_item(menu->get_selected_submenu()-1);
                return true;
            }
        } else if(dir == direction::down) {
            auto& menu = menus[selected];
            if(menu->get_selected_submenu() < menu->get_submenus_count()-1) {
                select_menu_item(menu->get_selected_submenu()+1);
                return true;
            }
        }
    } else {
        auto& menu = current_submenu;
        if(dir == direction::up) {
            if(menu->get_selected_submenu() > 0) {
                select_submenu_item(menu->get_selected_submenu()-1);
                return true;
            }
        } else if(dir == direction::down) {
            if(menu->get_selected_submenu() < menu->get_submenus_count()-1) {
                select_submenu_item(menu->get_selected_submenu()+1);
                return true;
            }
        }
    }
    return false;
}
bool main_menu::activate_current(action action) {
    auto& menu = *menus[selected];
    auto res = menu.activate(action);
    if(res == result::submenu) {
        auto& entry = current_submenu ? current_submenu->get_submenu(current_submenu->get_selected_submenu())
            : menu.get_submenu(menu.get_selected_submenu());
        if(auto submenu = dynamic_cast<menu::menu*>(&entry)) {
            if(submenu->get_submenus_count() > 0) {
                if(current_submenu) {
                    submenu_stack.push_back(current_submenu);
                }

                current_submenu = submenu;
                current_submenu->on_open();

                if(!in_submenu) {
                    in_submenu = true;
                    last_submenu_transition = std::chrono::system_clock::now();
                }
                return true;
            }
        }
        return false;
    }
    return res == result::success;
}
bool main_menu::back() {
    if(in_submenu) {
        current_submenu->on_close();
        if(submenu_stack.empty()) {
            current_submenu = nullptr;
            in_submenu = false;
            last_submenu_transition = std::chrono::system_clock::now();
        } else {
            current_submenu = submenu_stack.back();
            submenu_stack.pop_back();
        }
        return true;
    }
    return false;
}

void main_menu::select(int index) {
    if(index == selected) {
        return;
    }
    if(index < 0 || index >= menus.size()) {
        return;
    }

    last_selected = selected;
    last_selected_transition = std::chrono::system_clock::now();
    selected = index;

    menus[last_selected]->on_close();
    menus[selected]->on_open();

    last_selected_menu_item = menus[selected]->get_selected_submenu();
}
void main_menu::select_menu_item(int index) {
    auto& menu = menus[selected];
    if(index == menu->get_selected_submenu()) {
        return;
    }
    if(index < 0 || index >= menu->get_submenus_count()) {
        return;
    }

    last_selected_menu_item = menu->get_selected_submenu();
    last_selected_menu_item_transition = std::chrono::system_clock::now();
    menu->select_submenu(index);
}
void main_menu::select_submenu_item(int index) {
    auto& menu = current_submenu;
    if(index == menu->get_selected_submenu()) {
        return;
    }
    if(index < 0 || index >= menu->get_submenus_count()) {
        return;
    }

    last_selected_submenu_item = menu->get_selected_submenu();
    last_selected_submenu_item_transition = std::chrono::system_clock::now();
    menu->select_submenu(index);
}

void main_menu::render(dreamrender::gui_renderer& renderer) {
    constexpr glm::vec4 active_color(1.0f, 1.0f, 1.0f, 1.0f);
    constexpr glm::vec4 inactive_color(0.25f, 0.25f, 0.25f, 0.25f);

    auto now = std::chrono::system_clock::now();

    auto time_since_transition = std::chrono::duration<double>(now - last_submenu_transition);
    double partial = std::clamp(time_since_transition / transition_submenu_activate_duration, 0.0, 1.0);
    partial = in_submenu ? partial : 1.0 - partial;
    bool in_submenu_now = in_submenu || partial > 0.0;

    renderer.push_color(glm::mix(active_color, inactive_color, partial));
    render_crossbar(renderer, now);
    renderer.pop_color();

    std::vector<std::pair<action, std::string>> buttons{};
    buttons.reserve(5);
    menus[selected]->get_button_actions(buttons);
    if(in_submenu_now && current_submenu) {
        render_submenu(renderer, now);
        current_submenu->get_button_actions(buttons);
    }
    xmb->render_controller_buttons(renderer, 0.5f, 0.9f, buttons);
}

void main_menu::render_crossbar(dreamrender::gui_renderer& renderer, time_point now) {
    double submenu_transition = std::clamp(
        std::chrono::duration<double>(now - last_submenu_transition) / transition_submenu_activate_duration, 0.0, 1.0);
    submenu_transition = in_submenu ? submenu_transition : 1.0 - submenu_transition;
    bool in_submenu_now = in_submenu || submenu_transition > 0.0;

    const glm::vec2 base_pos = glm::mix(
        glm::vec2(0.35f/renderer.aspect_ratio, 0.25f),
        glm::vec2(0.30f/renderer.aspect_ratio, 0.25f),
        submenu_transition);
    const double base_size = glm::mix(0.1, 0.075, submenu_transition);

    float real_selection{};
    float selected_f = selected;
    float last_selected_f = last_selected;

    if(selected == last_selected) {
        real_selection = selected_f;
    } else {
        auto time_since_transition = std::chrono::duration<float>(now - last_selected_transition);
        real_selection = last_selected_f + (selected - last_selected) *
            time_since_transition / transition_duration;
        if(selected > last_selected && real_selection > selected_f) {
            real_selection = selected_f;
        } else if(selected < last_selected && real_selection < selected_f) {
            real_selection = selected_f;
        }
    }

    const auto selected_menu_x = base_pos.x;
    float x = selected_menu_x - (base_size*1.5f)/renderer.aspect_ratio*real_selection;

    for (int i = 0; i < menus.size(); i++) {
        if(i == selected && in_submenu_now) {
            x += (base_size*1.5f)/renderer.aspect_ratio;
            continue; // the selected menu is rendered as part of the submenu
        }

        auto& menu = menus[i];
        renderer.draw_image_a(menu->get_icon(), x, base_pos.y, base_size, base_size);
        if(i == selected) {
            renderer.draw_text(menu->get_name(), x+(base_size*0.5f)/renderer.aspect_ratio, base_pos.y+base_size, base_size*0.4f, glm::vec4(1, 1, 1, 1), true);
        }
        x += (base_size*1.5f)/renderer.aspect_ratio;
    }

    // This is spectacularly bad code, but it will work for now
    x = selected_menu_x - ((base_size*1.5f)/renderer.aspect_ratio)*(real_selection - selected_f);
    auto& menu = menus[selected];
    int selected_submenu = menu->get_selected_submenu();
    float partial_transition = 1.0f;
    float partial_y = 0.0f;
    if(selected_submenu != last_selected_menu_item) {
        auto time_since_transition = std::chrono::duration<double>(now - last_selected_menu_item_transition);
        if(time_since_transition > transition_menu_item_duration) {
            last_selected_menu_item = selected_submenu;
        } else {
            partial_transition = std::clamp(time_since_transition / transition_menu_item_duration, 0.0, 1.0);
            partial_y = (selected_submenu - last_selected_menu_item) * (1.0f - partial_transition);
        }
    }
    {
        float y = base_pos.y - (base_size*1.5f) + partial_y*base_size*1.5f;
        if(last_selected_menu_item > selected_submenu) {
            y += base_size*glm::mix(0.65f, 1.5f, 1.0f-partial_transition);
        } else if(last_selected_menu_item < selected_submenu) {
            y += base_size*glm::mix(-1.5f, 0.0f, partial_transition);
            y += base_size*glm::mix(0.65f, 0.0f, partial_transition);
            y += base_size*0.65f;
        } else {
            y += base_size*0.65f;
        }
        for(int i=selected_submenu-1; i >= 0 && y >= -base_size*0.65f; i--) {
            auto& submenu = menu->get_submenu(i);
            renderer.draw_image_a(submenu.get_icon(), x+(base_size*0.2f)/renderer.aspect_ratio, y, base_size*0.6f, base_size*0.6f);
            if(!in_submenu_now)
                renderer.draw_text(submenu.get_name(), x+(base_size*1.5f)/renderer.aspect_ratio, y+(base_size*0.3f), base_size*0.4f, glm::vec4(0.7, 0.7, 0.7, 1), false, true);
            y -= base_size*0.65f;
        }
    }
    {
        float y = base_pos.y + (base_size*1.5f) - (base_size*0.65f) + partial_y*base_size*1.5f;
        if(last_selected_menu_item > selected_submenu) {
            y += base_size*glm::mix(0.65f, 1.5f, 1.0f-partial_transition);
        } else {
            y += base_size*0.65f;
        }
        for(int i=selected_submenu, count = menu->get_submenus_count(); i<count && y < 1.0f; i++) {
            auto& submenu = menu->get_submenu(i);
            if(i == selected_submenu) {
                if(!in_submenu_now) {
                    double size = base_size*glm::mix(0.6, 1.2, partial_transition);
                double text_size = base_size*glm::mix(0.4, 0.6, partial_transition);
                    renderer.draw_image_a(submenu.get_icon(), x+(base_size*0.5f-size/2.0f)/renderer.aspect_ratio, y, size, size);
                    if(!in_submenu_now)
                        renderer.draw_text(submenu.get_name(), x+(base_size*1.5f)/renderer.aspect_ratio, y+size/2, text_size, glm::vec4(1, 1, 1, 1), false, true);
                }
                y += base_size*glm::mix(0.65f, 1.5f, partial_transition);
            }
            else if(i == last_selected_menu_item) {
                double size = base_size*glm::mix(0.6, 1.2, 1.0f-partial_transition);
                double text_size = base_size*glm::mix(0.4, 0.6, 1.0f-partial_transition);
                renderer.draw_image_a(submenu.get_icon(), x+(0.05f-size/2.0f)/renderer.aspect_ratio, y, size, size);
                if(!in_submenu_now)
                    renderer.draw_text(submenu.get_name(), x+(base_size*1.5f)/renderer.aspect_ratio, y+size/2, text_size, glm::vec4(1, 1, 1, 1), false, true);
                y += base_size*glm::mix(0.65f, 1.5f, 1.0f-partial_transition);
            }
            else {
                renderer.draw_image_a(submenu.get_icon(), x+(base_size*0.2f)/renderer.aspect_ratio, y, base_size*0.6f, base_size*0.6f);
                if(!in_submenu_now)
                    renderer.draw_text(submenu.get_name(), x+(base_size*1.5f)/renderer.aspect_ratio, y+base_size*0.3f, base_size*0.4f, glm::vec4(0.7, 0.7, 0.7, 1), false, true);
                y += base_size*0.65f;
            }
        }
    }
}

void main_menu::render_submenu(dreamrender::gui_renderer& renderer, time_point now) {
    double submenu_transition = std::clamp(
        std::chrono::duration<double>(now - last_submenu_transition) / transition_submenu_activate_duration, 0.0, 1.0);
    submenu_transition = in_submenu ? submenu_transition : 1.0 - submenu_transition;

    constexpr auto offset = (0.1f-0.075f)/2.0f;
    const glm::vec2 base_pos = glm::mix(
        glm::vec2(0.35f/renderer.aspect_ratio, 0.25f),
        glm::vec2((0.15f-offset)/renderer.aspect_ratio, 0.25f-2*offset),
        submenu_transition);
    const double base_size = 0.1;

    const auto& selected_menu = *menus[selected];
    const auto& selected_submenu = *current_submenu;

    renderer.draw_image_a(selected_menu.get_icon(), base_pos.x, base_pos.y, 0.1f, 0.1f);
    renderer.draw_image_a(selected_submenu.get_icon(), base_pos.x, base_pos.y+0.15f, 0.1f, 0.1f);

    if(!in_submenu)
        return;
    if(const auto* submenu = dynamic_cast<const menu::menu*>(&selected_submenu)) {
        double selected = submenu->get_selected_submenu();
        auto time_since_transition = std::chrono::duration<double>(now - last_selected_submenu_item_transition);
        if(time_since_transition < transition_submenu_item_duration) {
            selected = last_selected_submenu_item + (selected - last_selected_submenu_item) *
                time_since_transition / transition_submenu_item_duration;
        }

        double offsetY = 0.15f - selected*0.15f;

        for(int i=0; i<submenu->get_submenus_count(); i++) {
            double partial_selection = 0.0;
            if(i == selected) {
                partial_selection = std::clamp(time_since_transition / transition_submenu_item_duration, 0.0, 1.0);
            }

            double size = base_size*glm::mix(0.75, 1.0, partial_selection);
            double offset = (base_size - size) / 4.0;

            double y = base_pos.y+offsetY+0.15f*i;
            if(y < -size || y > 1.0+size)
                continue;

            auto& entry = submenu->get_submenu(i);
            renderer.draw_image_a(entry.get_icon(), base_pos.x + 0.1 + offset, y, size, size);
            renderer.draw_text(entry.get_name(), base_pos.x + 0.2, y+size/2, size/2, glm::vec4(1, 1, 1, 1), false, true);
            if(i == selected) {
                auto s = renderer.measure_text(entry.get_name(), size/2);
                renderer.draw_text(entry.get_description(), base_pos.x + 0.2, y+size/2 + s.y, size / 3);
            }
        }
    }
}

}
