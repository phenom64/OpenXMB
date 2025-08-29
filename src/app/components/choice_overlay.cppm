module;

#include <chrono>
#include <functional>
#include <string>
#include <vector>

export module openxmb.app:choice_overlay;

import dreamrender;
import sdl2;
import vulkan_hpp;
import vma;
import openxmb.utils;
import :component;
import glm;

namespace app {

export class choice_overlay : public component, public action_receiver {
    public:
        choice_overlay(std::vector<std::string> choices, unsigned int selection_index = 0,
            std::function<void(unsigned int)> confirm_callback = [](unsigned int){},
            std::function<void()> cancel_callback = [](){}
        );
        // Optional colour swatches to display next to each choice (same length as choices or empty)
        void set_colour_swatches(const std::vector<glm::vec3>& cols) { swatches = cols; }

        void render(dreamrender::gui_renderer& renderer, class shell* xmb) override;
        result on_action(action action) override;

        [[nodiscard]] bool is_opaque() const override { return false; }
        [[nodiscard]] bool do_fade_in() const override { return true; }
        [[nodiscard]] bool do_fade_out() const override { return true; }
    private:
        using time_point = std::chrono::time_point<std::chrono::system_clock>;

        std::vector<std::string> choices;
        std::function<void(unsigned int)> confirm_callback;
        std::function<void()> cancel_callback;

        bool select_relative(action dir);

        unsigned int selection_index = 0;
        unsigned int last_selection_index = 0;
        time_point last_selection_time = std::chrono::system_clock::now();

        constexpr static auto transition_duration = std::chrono::milliseconds(100);

        std::vector<glm::vec3> swatches;
};

}
