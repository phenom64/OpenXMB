module;

export module shell.app:component;

import dreamrender;
import shell.utils;
import vulkan_hpp;

namespace app {

class shell;

export class component {
    public:
        virtual ~component() = default;
        virtual void prerender(vk::CommandBuffer cmd, int frame, app::shell* xmb) {};
        virtual void render(dreamrender::gui_renderer& renderer, app::shell* xmb) = 0;
        virtual result tick(app::shell* xmb) { return result::success; }
        [[nodiscard]] virtual bool is_opaque() const { return true; }
        [[nodiscard]] virtual bool do_fade_in() const { return false; }
        [[nodiscard]] virtual bool do_fade_out() const { return false; }
};

}
