module;

#include <functional>
#include <string>

export module xmbshell.app:message_overlay;

import dreamrender;
import sdl2;
import vulkan_hpp;
import vma;
import xmbshell.utils;
import :component;

namespace app {

export class message_overlay : public component, public action_receiver {
    public:
        message_overlay(std::string title, std::string message, std::vector<std::string> choices = {},
            std::function<void(unsigned int)> confirm_callback = [](unsigned int){},
            bool cancelable = true, std::function<void()> cancel_callback = [](){}
        );

        void render(dreamrender::gui_renderer& renderer, class xmbshell* xmb) override;
        result on_action(action action) override;
    private:
        std::string title;
        std::string message;
        std::vector<std::string> choices;
        std::function<void(unsigned int)> confirm_callback;
        bool cancelable;
        std::function<void()> cancel_callback;

        unsigned int selected = 0;
};

}
