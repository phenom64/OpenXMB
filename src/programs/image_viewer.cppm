module;

#include <cassert>
#include <filesystem>
#include <future>

export module shell.app:image_viewer;

import dreamrender;
import glm;
import spdlog;
import shell.utils;
import :component;
import :programs;
import :base_viewer;

namespace programs {

using namespace app;

export class image_viewer : private base_viewer, public component, public action_receiver, public joystick_receiver, public mouse_receiver {
    public:
        image_viewer(std::filesystem::path path, dreamrender::resource_loader& loader) : path(std::move(path)) {
            texture = std::make_shared<dreamrender::texture>(loader.getDevice(), loader.getAllocator());
            load_future = loader.loadTexture(texture.get(), this->path);
        }
        image_viewer(std::shared_ptr<dreamrender::texture> texture) : texture(std::move(texture)) {
        }

        result tick(shell*) override {
            if(!texture->loaded) {
                return result::success;
            }
            image_width = texture->width;
            image_height = texture->height;

            base_viewer::tick();
            return result::success;
        }

        void render(dreamrender::gui_renderer& renderer, class shell* xmb) override {
            if(!texture->loaded) {
                return;
            }
            constexpr float size = 0.9;
            base_viewer::render(texture->imageView.get(), size, renderer);
        }
        result on_action(action action) override {
            if(action == action::cancel) {
                return result::close;
            }
            else {
                result r = base_viewer::on_action(action);
                if(r != result::unsupported) {
                    return r;
                }
            }
            return result::failure;
        }

        result on_joystick(unsigned int index, float x, float y) override {
            return base_viewer::on_joystick(index, x, y);
        }

        result on_mouse_move(float x, float y) override {
            return base_viewer::on_mouse_move(x, y);
        }

        [[nodiscard]] bool is_opaque() const override {
            // This is probably undefined behavior/a race condition, but it works well enough.
            // Maybe one day the entire program will explode due to this, oh well!
            return texture->loaded;
        }
    private:
        std::filesystem::path path;
        std::shared_ptr<dreamrender::texture> texture;
        std::shared_future<void> load_future;
};

namespace {
const inline register_program<image_viewer> image_viewer_program{
    "image_viewer",
    {
        "image/bmp", "image/gif", "image/jpeg", "image/vnd.zbrush.pcx",
        "image/png", "image/x-portable-bitmap", "image/x-portable-graymap",
        "image/x-portable-pixmap", "image/x-portable-anymap", "image/svg+xml",
        "image/x-targa", "image/x-tga", "image/tiff", "image/webp", "image/x-xcf",
        "image/x-xpixmap",
    },
    {
        ".bmp", ".gif", ".jpg", ".jpeg", ".pcx", ".png",
        ".pbm", ".pgm", ".ppm", ".pnm", ".svg", ".tga", ".tiff",
        ".webp", ".xcf", ".xpm"
    }
};
}

}