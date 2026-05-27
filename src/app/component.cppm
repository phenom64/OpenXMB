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

#include <span>
#include <string_view>
#include <utility>
#include <variant>

export module openxmb.app:component;

import dreamrender;
import openxmb.utils;
import vulkan_hpp;

namespace app {

class shell;

export namespace events {
    enum class logical_controller_button {
        invalid = -1,
        a = 0,
        b = 1,
        x = 2,
        y = 3,
        back = 4,
        guide = 5,
        start = 6,
        leftstick = 7,
        rightstick = 8,
        leftshoulder = 9,
        rightshoulder = 10,
        dpad_up = 11,
        dpad_down = 12,
        dpad_left = 13,
        dpad_right = 14,
        misc1 = 15,
        paddle1 = 16,
        paddle2 = 17,
        paddle3 = 18,
        paddle4 = 19,
        touchpad = 20,
    };

    struct controller_button_down {
        logical_controller_button button;
    };
    struct controller_button_up {
        logical_controller_button button;
    };

    enum class logical_joystick_index {
        left = 0,
        right = 1
    };
    struct joystick_axis {
        logical_joystick_index index;
        float x;
        float y;

        joystick_axis(unsigned int index, float x, float y) :
            index(static_cast<logical_joystick_index>(index)), x(x), y(y) {}
        joystick_axis(logical_joystick_index index, float x, float y) :
            index(index), x(x), y(y) {}
    };

    struct mouse_move {
        float x;
        float y;
        float xrel;
        float yrel;
    };
    struct mouse_scroll {
        float x;
    };

    enum class logical_mouse_button {
        left,
        middle,
        right,
        x1,
        x2
    };
    struct mouse_button_down {
        logical_mouse_button button;
    };
    struct mouse_button_up {
        logical_mouse_button button;
    };

    struct key_down {
        unsigned int keycode;
    };
    struct key_up {
        unsigned int keycode;
    };

    struct cursor_move {
        float x;
        float y;
    };
}

export struct event {
    ::action action = ::action::none;

    std::variant<std::monostate,
                 events::controller_button_down, events::controller_button_up,
                 events::joystick_axis,
                 events::mouse_move, events::mouse_scroll, events::mouse_button_down, events::mouse_button_up,
                 events::key_down, events::key_up,
                 events::cursor_move
                > data;

    template<typename T>
    bool is() const {
        return std::holds_alternative<T>(data);
    }

    template<typename T>
    T* get() {
        return std::get_if<T>(&data);
    }

    template<typename T>
    const T* get() const {
        return std::get_if<T>(&data);
    }

    template<typename T, typename Pred>
    bool test(Pred pred, bool default_value = false) const {
        if(auto* d = std::get_if<T>(&data)) {
            return pred(*d);
        }
        return default_value;
    }
};

export class event_receiver {
    public:
        virtual ~event_receiver() = default;
        virtual result on_event(const event&) {
            return result::unsupported;
        }
};

export class component {
    public:
        virtual ~component() = default;
        virtual void prerender(vk::CommandBuffer cmd, int frame, app::shell* xmb) {};
        virtual void render(dreamrender::gui_renderer& renderer, app::shell* xmb) = 0;
        virtual result tick(app::shell* xmb) { return result::success; }
        [[nodiscard]] virtual bool is_opaque() const { return true; }
        [[nodiscard]] virtual bool do_fade_in() const { return false; }
        [[nodiscard]] virtual bool do_fade_out() const { return false; }
        [[nodiscard]] virtual bool enable_cursor() const { return false; }
    protected:
        void render_controller_buttons(app::shell* xmb, dreamrender::gui_renderer& renderer, float x, float y, std::span<const std::pair<::action, std::string_view>> buttons) const;
};

}
