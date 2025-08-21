module;

#include <cmath>

export module openxmb.app:base_viewer;

import glm;
import dreamrender;
import vulkan_hpp;
import openxmb.utils;

namespace programs {

export class base_viewer {
    protected:
        void tick() {
            float pic_aspect = static_cast<float>(image_width) / static_cast<float>(image_height);
            glm::vec2 limit(pic_aspect, 1.0f);
            limit *= zoom;
            limit /= 2.0f;

            offset = glm::clamp(offset + move_delta_pos, -limit, limit);
        }

        void render(vk::ImageView view, float size, dreamrender::gui_renderer& renderer) {
            float bw = size*renderer.aspect_ratio;
            float bh = size;

            float pic_aspect = image_width / (float)image_height;
            float w{}, h{};
            if(pic_aspect > renderer.aspect_ratio) {
                w = bw;
                h = bw / pic_aspect;
            } else {
                h = bh;
                w = bh * pic_aspect;
            }
            glm::vec2 offset = {(1.0f-size)/2, (1.0f-size)/2};
            offset += glm::vec2((bw-w)/2/renderer.aspect_ratio, (bh-h)/2);
            offset -= (zoom-1.0f) * glm::vec2(w/renderer.aspect_ratio, h) / 2.0f;
            offset += this->offset / glm::vec2(renderer.aspect_ratio, 1.0f);

            renderer.set_clip((1.0f-size)/2, (1.0f-size)/2, size, size);
            renderer.draw_image(view, offset.x, offset.y, zoom*w, zoom*h);
            renderer.reset_clip();
        }

        result on_action(action action) {
            if(action == action::up) {
                zoom *= 1.1f;
                return result::success;
            }
            else if(action == action::down) {
                zoom /= 1.1f;
                return result::success;
            }
            else if(action == action::extra) {
                offset = {0.0f, 0.0f};
                zoom = 1.0f;
                return result::success;
            }
            return result::unsupported;
        }

        result on_joystick(unsigned int index, float x, float y) {
            if(index == 1) {
                move_delta_pos = -glm::vec2(x, y)/25.0f;
                if(std::abs(x) < 0.1f) {
                    move_delta_pos.x = 0.0f;
                }
                if(std::abs(y) < 0.1f) {
                    move_delta_pos.y = 0.0f;
                }
                return result::success;
            }
            return result::unsupported;
        }

        result on_mouse_move(float x, float y) {
            offset = glm::vec2(x, y) - glm::vec2(0.5f, 0.5f);
            return result::success;
        }

        unsigned int image_width, image_height;

        glm::vec2 move_delta_pos = {0.0f, 0.0f};

        float zoom = 1.0f;
        glm::vec2 offset = {0.0f, 0.0f};
};

}