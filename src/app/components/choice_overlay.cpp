module;

#include <array>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

module openxmb.app;
import :choice_overlay;

import dreamrender;
import glm;
import vulkan_hpp;
import vma;

namespace app {

choice_overlay::choice_overlay(std::vector<std::string> choices, unsigned int selection_index,
    std::function<void(unsigned int)> confirm_callback, std::function<void()> cancel_callback)
    : choices{std::move(choices)}, selection_index(selection_index), last_selection_index{selection_index},
      confirm_callback{confirm_callback}, cancel_callback{cancel_callback}
{

}

result choice_overlay::on_action(action action) {
    switch(action) {
        case action::cancel:
            if(cancel_callback) {
                cancel_callback();
            }
            return result::close;
        case action::ok:
            if(confirm_callback) {
                confirm_callback(selection_index);
            }
            return result::close;
        case action::up:
            return select_relative(action::up) ? result::success | result::ok_sound : result::unsupported | result::error_rumble;
        case action::down:
            return select_relative(action::down) ? result::success | result::ok_sound : result::unsupported | result::error_rumble;
        default:
            return result::unsupported | result::error_rumble;
    }
}

bool choice_overlay::select_relative(action dir) {
    if(dir == action::up) {
        if(selection_index <= 0) {
            return false;
        }
        last_selection_index = selection_index;
        last_selection_time = std::chrono::system_clock::now();
        selection_index = (selection_index + choices.size() - 1) % choices.size();
    } else if(dir == action::down) {
        if(selection_index >= choices.size() - 1) {
            return false;
        }
        last_selection_index = selection_index;
        last_selection_time = std::chrono::system_clock::now();
        selection_index = (selection_index + 1) % choices.size();
    } else {
        return false;
    }
    return true;
}

void choice_overlay::render(dreamrender::gui_renderer& renderer, class shell* xmb) {
    renderer.draw_quad(std::array{
        dreamrender::simple_renderer::vertex_data{{0.65f, 0.0f}, {0.1f, 0.1f, 0.1f, 1.0f}, {0.0f, 0.0f}},
        dreamrender::simple_renderer::vertex_data{{0.65f, 1.0f}, {0.1f, 0.1f, 0.1f, 1.0f}, {0.0f, 1.0f}},
        dreamrender::simple_renderer::vertex_data{{0.90f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        dreamrender::simple_renderer::vertex_data{{0.90f, 1.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
    }, dreamrender::simple_renderer::params{
        std::array{
            glm::vec2{0.0f, 0.05f},
            glm::vec2{0.0f, 0.0f},
            glm::vec2{-0.1f, 0.1f},
            glm::vec2{-0.1f, 0.1f},
        }
    });

    auto now = std::chrono::system_clock::now();
    double selected = selection_index;
    auto time_since_transition = std::chrono::duration<double>(now - last_selection_time);
    if(time_since_transition < transition_duration) {
        selected = last_selection_index + (selected - last_selection_index) *
            time_since_transition / transition_duration;
    }

    constexpr double base_size = 0.075;
    constexpr double item_height = 0.05;
    constexpr glm::vec2 base_pos = {0.675f, 0.425f};

    double offsetY = -selected*item_height;

    for(int i=0; i<choices.size(); i++) {
        double partial_selection = 0.0;
        if(i == selected) {
            partial_selection = std::clamp(time_since_transition / transition_duration, 0.0, 1.0);
        }

        double size = base_size*glm::mix(0.75, 1.0, partial_selection);

        const std::string& entry = choices[i];
        renderer.draw_text(entry, base_pos.x, base_pos.y + offsetY + item_height*i, size, glm::vec4(1, 1, 1, 1), false, true);
    }
}

}
