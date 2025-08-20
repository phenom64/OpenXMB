module;

export module shell.app:news_display;

import dreamrender;
import vulkan_hpp;
import vma;

namespace app {

class news_display {
    public:
        news_display(class shell* shell);
        void preload(vk::Device device, vma::Allocator allocator, dreamrender::resource_loader& loader);
        void tick();
        void render(dreamrender::gui_renderer& renderer);
    private:
        class shell* shell;
};

}
