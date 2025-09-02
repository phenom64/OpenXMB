/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025 Syndromatic Ltd. All rights reserved
 * Designed by Kavish Krishnakumar in Manchester.
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
#include <string>

export module openxmb.app:startup_overlay;

import dreamrender;
import glm;
import sdl2;
import spdlog;
import openxmb.config;
import :component;

namespace app {

// Simple boot splash: plays startup jingle and fades text in/out
export class startup_overlay : public component {
  public:
    startup_overlay() = default;
    ~startup_overlay() override = default;

    result tick(app::shell*) override;
    void render(dreamrender::gui_renderer& renderer, app::shell*) override;
    // Opaque so menus do not render behind the intro (PS3-like behavior)
    [[nodiscard]] bool is_opaque() const override { return true; }
    [[nodiscard]] bool do_fade_in() const override { return true; }
    [[nodiscard]] bool do_fade_out() const override { return true; }

  private:
    using time_point = std::chrono::time_point<std::chrono::system_clock>;
    time_point start_time { std::chrono::system_clock::now() };
    bool started_audio { false };
};

}
