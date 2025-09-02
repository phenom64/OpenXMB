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

#include <chrono>
#include <cmath>
#include <string_view>

module openxmb.app;

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