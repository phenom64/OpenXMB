module;

#include <chrono>
#include <cmath>
#include <string_view>

module shell.app;

import dreamrender;
import vulkan_hpp;
import vma;

namespace app {

news_display::news_display(class shell* shell) : shell(shell) {}

void news_display::preload(vk::Device device, vma::Allocator allocator, dreamrender::resource_loader& loader) {
}

void news_display::tick() {
}

void news_display::render(dreamrender::gui_renderer& renderer) {
    tick();

    constexpr float base_x = 0.75f;
    constexpr float base_y = 0.15f;
    constexpr float box_width = 0.15f;
    constexpr float font_size = 0.021296296f*2.5f;
    constexpr float speed = 0.05f;
    constexpr float spacing = 0.025f;

    static auto begin = std::chrono::system_clock::now();
    auto now = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration<float>(now - begin).count() * speed;

    std::string_view news = "Lorem ipsum dolor sit amet, consectetur adipiscing elit";
    float width = renderer.measure_text(news, font_size).x;
    float x = std::fmod(elapsed, width + spacing);

    renderer.set_clip(base_x, base_y, box_width, font_size);

    renderer.draw_text(news, base_x+box_width-x, base_y, font_size);
    renderer.draw_text(news, base_x+box_width-(x+width+spacing), base_y, font_size);

    renderer.reset_clip();
}

}
