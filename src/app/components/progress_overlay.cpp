module;

#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

module openxmb.app;
import :progress_overlay;

import :message_overlay;

import dreamrender;
import glm;
import spdlog;

namespace app {
    progress_overlay::progress_overlay(std::string title, std::unique_ptr<progress_item>&& item, bool show_progress)
        : title(std::move(title)), item(std::move(item)), show_progress(show_progress)
    {
        auto status = this->item->init(status_message);
        if(status == progress_item::status::error) {
            failed = true;
        } else if(status == progress_item::status::success) {
            done = true;
        }
    }

    result progress_overlay::tick(shell* xmb) {
        auto status = item->progress(progress, status_message);
        if(status == progress_item::status::error) {
            failed = true;
        } else if(status == progress_item::status::success) {
            done = true;
        }

        if(failed) {
            spdlog::error("Progress failed: \"{}\"", status_message);
            if(!status_message.empty()) {
                xmb->emplace_overlay<message_overlay>(title, status_message, std::vector<std::string>{"OK"});
            }
            return result::failure | result::close;
        }
        if(done) {
            spdlog::info("Progress done: \"{}\"", status_message);
            if(!status_message.empty()) {
                xmb->emplace_overlay<message_overlay>(title, status_message, std::vector<std::string>{"OK"});
            }
            return result::success | result::close;
        }
        return result::success;
    }

    void progress_overlay::render(dreamrender::gui_renderer& renderer, shell* xmb) {
        renderer.draw_rect(glm::vec2(0.0f, 0.15f), glm::vec2(1.0f, 2.0/renderer.frame_size.height), glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
        renderer.draw_rect(glm::vec2(0.0f, 0.85f), glm::vec2(1.0f, 2.0/renderer.frame_size.height), glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));

        renderer.draw_text(title, 0.075f, 0.125f, 0.05f);
        xmb->render_controller_buttons(renderer, 0.5f, 0.9f, std::array{
            std::pair{action::none, ""},
            std::pair{action::cancel, "Back"}
        });

        float total_height = 0;
        float total_width = 0;
        {
            std::string_view text = status_message;
            for(const auto line : std::views::split(text, '\n')) {
                glm::vec2 size = renderer.measure_text(std::string_view(line), 0.05f);
                total_height += size.y;
                total_width = std::max(total_width, size.x);
            }
        }
        float y = 0.425f - total_height;
        for(const auto line : std::views::split(status_message, '\n')) {
            std::string_view sv(line);
            renderer.draw_text(sv, 0.5f-total_width/2, y, 0.05f, glm::vec4(1.0));
            y += renderer.measure_text(sv, 0.05f).y;
        }

        if(show_progress) {
            simple_renderer::params border_radius{
                {}, {0.5f, 0.5f, 0.5f, 0.5f}
            };
            simple_renderer::params blur{
                std::array{glm::vec2{0.0f, 0.0f}, glm::vec2{0.0f, 0.0f}, glm::vec2{0.0f, 0.5f}, glm::vec2{0.0f, 0.5f}},
                {0.5f, 0.5f, 0.5f, 0.5f}
            };
            renderer.draw_rect(glm::vec2(0.25f, 0.465f), glm::vec2(0.5f, 0.01f), glm::vec4(0.2f, 0.2f, 0.2f, 1.0f), border_radius);
            renderer.draw_rect(glm::vec2(0.25f, 0.465f), glm::vec2(0.5f, 0.01f), glm::vec4(0.1f, 0.1f, 0.1f, 1.0f), blur);

            constexpr glm::vec2 padding = glm::vec2{0.001f, 0.001f};
            renderer.draw_rect(glm::vec2(0.25f, 0.465f)+padding, glm::vec2(progress*0.5f, 0.01f)-2.0f*padding,
                glm::vec4(0x83/255.0f, 0x8d/255.0f, 0x22/255.0f, 1.0f), border_radius); // #838d22
            renderer.draw_rect(glm::vec2(0.25f, 0.465f)+padding, glm::vec2(progress*0.5f, 0.01f)-2.0f*padding,
                glm::vec4(1.0f, 1.0f, 1.0f, 0.1f), blur);

            renderer.draw_text(std::format("{:.0f}%", progress*100.0), 0.5f, 0.5f, 0.05f, glm::vec4(1.0), true, true);
        }
    }

    result progress_overlay::on_action(action action) {
        switch(action) {
            case action::cancel:
                return item->cancel(status_message) ? result::success | result::close : result::unsupported | result::error_rumble;
            default:
                return result::unsupported;
        }
    }
}
