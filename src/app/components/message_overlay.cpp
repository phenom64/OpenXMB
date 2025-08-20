module;

#include <functional>
#include <ranges>
#include <string>

module shell.app;
import :message_overlay;

import dreamrender;
import glm;
import spdlog;

namespace app {
    message_overlay::message_overlay(std::string title, std::string message, std::vector<std::string> choices,
        std::function<void(unsigned int)> confirm_callback, bool cancelable, std::function<void()> cancel_callback
    ) : title(std::move(title)), message(std::move(message)), choices(std::move(choices)),
        confirm_callback(std::move(confirm_callback)), cancelable(cancelable), cancel_callback(std::move(cancel_callback))
    {
    }

    void message_overlay::render(dreamrender::gui_renderer& renderer, shell* xmb) {
        renderer.draw_rect(glm::vec2(0.0f, 0.15f), glm::vec2(1.0f, 2.0/renderer.frame_size.height), glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
        renderer.draw_rect(glm::vec2(0.0f, 0.85f), glm::vec2(1.0f, 2.0/renderer.frame_size.height), glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));

        renderer.draw_text(title, 0.075f, 0.125f, 0.05f);
        xmb->render_controller_buttons(renderer, 0.5f, 0.9f, std::array{
            std::pair{action::ok, "Enter"},
            std::pair{cancelable ? action::cancel : action::none, "Back"}
        });

        {
            float total_height = 0;
            float total_width = 0;
            for(const auto line : std::views::split(message, '\n')) {
                glm::vec2 size = renderer.measure_text(std::string_view(line), 0.05f);
                total_height += size.y;
                total_width = std::max(total_width, size.x);
            }
            float y = 0.45f - total_height;
            for(const auto line : std::views::split(message, '\n')) {
                std::string_view sv(line);
                renderer.draw_text(sv, 0.5f-total_width/2, y, 0.05f, glm::vec4(1.0));
                y += renderer.measure_text(sv, 0.05f).y;
            }
        }
        {
            constexpr float gap = 0.025f;
            float total_width = 0;
            for(const auto& choice : choices) {
                glm::vec2 size = renderer.measure_text(choice, 0.05f);
                total_width += size.x + gap;
            }
            total_width -= gap;
            float x = 0.5f - total_width/2;
            for(unsigned int i=0; i<choices.size(); i++) {
                const auto& choice = choices[i];
                auto size = renderer.measure_text(choice, 0.05f);
                if(i == selected) {
                    constexpr glm::vec2 padding{0.010f, 0.005f};
                    float aspect_ratio = (size.x+padding.x) / (size.y+padding.y);
                    simple_renderer::params params = {
                        .blur = {
                            glm::vec2{-0.01f, 0.4f}/aspect_ratio, glm::vec2{-0.01f, 0.4f}/aspect_ratio,
                            glm::vec2{-0.1f, 0.4f}, glm::vec2{-0.1f, 0.4f}
                        },
                        .border_radius = {0.25f, 0.25f, 0.25f, 0.25f},
                        .aspect_ratio = static_cast<float>(aspect_ratio * renderer.aspect_ratio)
                    };
                    renderer.draw_rect(glm::vec2(x, 0.5f-size.y/2)-padding, size+padding*2.0f,
                        glm::vec4(0.5f, 0.5f, 0.5f, 0.33f), params);
                }
                renderer.draw_text(choice, x, 0.5f, 0.05f, glm::vec4(1.0), false, true);
                x += size.x + gap;
            }
        }
    }

    result message_overlay::on_action(action action) {
        switch(action) {
            case action::cancel:
                if(cancelable) {
                    if(cancel_callback) {
                        cancel_callback();
                    }
                    return result::success | result::close;
                }
                return result::unsupported;
            case action::ok:
                if(confirm_callback) {
                    confirm_callback(selected);
                }
                return result::success | result::close;
            case action::left:
                if(selected > 0) {
                    selected--;
                    return result::success | result::ok_sound;
                }
                return result::unsupported | result::error_rumble;
            case action::right:
                if(selected < choices.size() - 1) {
                    selected++;
                    return result::success | result::ok_sound;
                }
                return result::unsupported | result::error_rumble;
            default:
                return result::unsupported;
        }
    }
}
