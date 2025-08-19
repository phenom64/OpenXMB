module;

export module xmbshell.app:component;

import dreamrender;
import xmbshell.utils;
import vulkan_hpp;

namespace app {

class xmbshell;

export class component {
    public:
        virtual ~component() = default;
        virtual void prerender(vk::CommandBuffer cmd, int frame, app::xmbshell* xmb) {};
        virtual void render(dreamrender::gui_renderer& renderer, app::xmbshell* xmb) = 0;
        virtual result tick(app::xmbshell* xmb) { return result::success; }
        [[nodiscard]] virtual bool is_opaque() const { return true; }
        [[nodiscard]] virtual bool do_fade_in() const { return false; }
        [[nodiscard]] virtual bool do_fade_out() const { return false; }
};

}
