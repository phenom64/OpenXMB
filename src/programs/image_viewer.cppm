/* XMBShell, a console-like desktop shell
 * Copyright (C) 2025 - JCM
 *
 * This file (or substantial portions of it) is derived from XMBShell:
 *   https://github.com/JnCrMx/xmbshell
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

#include <array>
#include <cassert>
#include <filesystem>
#include <future>
#include <string_view>

export module openxmb.app:image_viewer;

import dreamrender;
import glm;
import spdlog;
import openxmb.utils;
import :component;
import :programs;
import :base_viewer;

namespace programs {

using namespace app;

export class image_viewer : private base_viewer, public component, public action_receiver, public event_receiver, public joystick_receiver, public mouse_receiver {
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
            render_controller_buttons(xmb, renderer, 0.5f, 0.95f, std::array{
                std::pair{action::up, std::string_view{"Zoom In"}},
                std::pair{action::down, std::string_view{"Zoom Out"}},
                std::pair{action::extra, std::string_view{"Reset"}},
                std::pair{action::cancel, std::string_view{"Close"}},
            });

            constexpr float size = 0.875f;
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

        result on_event(const event& event) override {
            if(auto* d = event.get<events::joystick_axis>()) {
                return base_viewer::on_joystick(static_cast<unsigned int>(d->index), d->x, d->y);
            }
            if(auto* d = event.get<events::mouse_move>()) {
                return base_viewer::on_mouse_move(d->x, d->y);
            }
            return on_action(event.action);
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
