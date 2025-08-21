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
    [[nodiscard]] bool is_opaque() const override { return false; }

  private:
    using time_point = std::chrono::time_point<std::chrono::system_clock>;
    time_point start_time { std::chrono::system_clock::now() };
    bool started_audio { false };
};

}

