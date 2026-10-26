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

#include <openxmb/photos/photo_library.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module openxmb.app:image_viewer;

import dreamrender;
import glm;
import sdl2;
import vulkan_hpp;
import openxmb.config;
import openxmb.utils;
import :component;
import :programs;
import :base_viewer;
import :choice_overlay;
import :message_overlay;
import openxmb.app;

namespace programs {
using namespace app;
namespace photo = openxmb::photos;

export class image_viewer : private base_viewer, public component,
    public action_receiver, public event_receiver, public joystick_receiver,
    public mouse_receiver {
public:
    float typography_scale() const override { return 2.5F; }

    image_viewer(std::filesystem::path selected, dreamrender::resource_loader& resources)
        : path(std::filesystem::absolute(std::move(selected)).lexically_normal()), loader(&resources) {
        image_width = image_height = 1;
        request_load();
    }
    image_viewer(std::shared_ptr<dreamrender::texture> image) : texture(std::move(image)) {
        image_width = image_height = 1;
    }
    ~image_viewer() override {
        // Upload jobs own their pixel buffers; CPU tasks never capture this viewer.
        // Keep GPU resources alive until submitted draws have finished.
        if(texture || incoming) {
            try { wait_gpu(); } catch(...) {}
        }
    }

    result tick(shell* shell) override {
        xmb = shell;
        const auto now = clock::now();
        const auto elapsed = std::clamp(std::chrono::duration<float>(now-last_tick).count(), 0.0f, 0.1f);
        last_tick = now;
        if(!cropping) {
            offset += move_delta_pos * elapsed * 60.0f;
            offset = glm::clamp(offset, glm::vec2(-zoom), glm::vec2(zoom));
        }
        if(pending_action) {
            const auto command = *pending_action;
            pending_action.reset();
            perform(command);
        }
        if(decode.valid() && ready(decode)) {
            try {
                auto loaded = decode.get();
                if(loaded.generation == generation) {
                    info = std::move(loaded.info);
                    if(!loaded.images.empty()) {
                        images = std::move(loaded.images);
                        auto found = std::find(images.begin(), images.end(), path);
                        selected_index = found == images.end() ? 0 : static_cast<std::size_t>(found-images.begin());
                    }
                    auto pixels = std::make_shared<photo::Bitmap>(std::move(loaded.image));
                    incoming = std::make_shared<dreamrender::texture>(loader->getDevice(), loader->getAllocator(), pixels->width, pixels->height);
                    upload_generation=generation;
                    upload = loader->loadTexture(incoming.get(), dreamrender::LoaderFunction{
                        [pixels](std::uint8_t* destination, std::size_t capacity) {
                            if(pixels->rgba.size() > capacity) throw std::runtime_error("Image exceeds texture upload limit");
                            std::memcpy(destination, pixels->rgba.data(), pixels->rgba.size());
                        }});
                }
            } catch(const std::exception& error) {
                message = error.what();
                error_state = true;
                if(texture) { wait_gpu(); texture.reset(); }
            }
            if(load_requested) begin_load();
        }
        if(upload.valid() && ready(upload)) {
            try {
                upload.get();
                if(incoming && incoming->loaded.load(std::memory_order_acquire) && upload_generation==generation) {
                    if(texture) wait_gpu();
                    texture = std::move(incoming);
                    image_width = texture->width; image_height = texture->height;
                    message.clear(); error_state = false; last_slide = now;
                }
            } catch(const std::exception& error) { message = error.what(); error_state = true; incoming.reset(); }
            if(load_requested && !decode.valid()) begin_load();
        }
        if(texture && texture->loaded.load(std::memory_order_acquire)) {
            image_width=texture->width; image_height=texture->height;
        }
        if(operation.valid() && ready(operation)) {
            try {
                auto completed=operation.get();
                if(completed.kind == operation_kind::trash) {
                    const auto deleted=path;
                    images.erase(std::remove(images.begin(),images.end(),deleted),images.end());
                    if(images.empty()) close_pending=true;
                    else { selected_index=std::min(selected_index,images.size()-1); path=images[selected_index]; turns=0; request_load(); }
                    notify("Photo moved to OpenXMB Trash", "The original is recoverable at:\n"+completed.path.string());
                } else if(completed.kind == operation_kind::wallpaper) {
                    apply_wallpaper(completed.path);
                } else {
                    notify("Photo copy saved", completed.path.string()+"\nThe original was preserved.");
                    // Discover the newly saved copy when the next gallery scan runs.
                    images=photo::gallery(path);
                    auto found=std::find(images.begin(),images.end(),path);
                    if(found!=images.end()) selected_index=static_cast<std::size_t>(found-images.begin());
                }
            } catch(const std::exception& error) { notify("Photo operation failed",error.what()); }
        }
        if(slideshow && !modal_open && !cropping && !busy() && images.size()>1 && now-last_slide>=slide_interval) navigate(1);
        if(close_pending && !modal_open) return result::close;
        return result::success;
    }

    void render(dreamrender::gui_renderer& renderer, shell*) override {
        view_aspect=static_cast<float>(renderer.aspect_ratio);
        renderer.draw_rect(glm::vec2(0),glm::vec2(1),glm::vec4(0.015f,0.018f,0.025f,1));
        std::string title=path.empty() ? "Photo" : path.filename().string();
        if(!images.empty()) title += "   " + std::to_string(selected_index+1)+" / "+std::to_string(images.size());
        renderer.draw_text(title,0.045f,0.035f,0.035f);
        if(texture && texture->loaded.load(std::memory_order_acquire) && !error_state) {
            const auto geometry=layout(static_cast<float>(renderer.aspect_ratio));
            renderer.set_clip(0.035f,0.09f,0.93f,0.76f);
            renderer.draw_image(*texture,geometry[0],geometry[1],geometry[2]*renderer.aspect_ratio,geometry[3]);
            if(cropping) {
                const float left=geometry[0]+geometry[2]*crop_region.x;
                const float top=geometry[1]+geometry[3]*crop_region.y;
                const float width=geometry[2]*crop_region.width;
                const float height=geometry[3]*crop_region.height;
                const glm::vec4 tint(0.0f,0.0f,0.0f,0.55f);
                renderer.draw_rect(glm::vec2(geometry[0],geometry[1]),glm::vec2(geometry[2],top-geometry[1]),tint);
                renderer.draw_rect(glm::vec2(geometry[0],top+height),glm::vec2(geometry[2],geometry[1]+geometry[3]-top-height),tint);
                renderer.draw_rect(glm::vec2(geometry[0],top),glm::vec2(left-geometry[0],height),tint);
                renderer.draw_rect(glm::vec2(left+width,top),glm::vec2(geometry[0]+geometry[2]-left-width,height),tint);
                const float px=2.0f/renderer.frame_size.width, py=2.0f/renderer.frame_size.height;
                renderer.draw_rect(glm::vec2(left,top),glm::vec2(width,py),glm::vec4(1));
                renderer.draw_rect(glm::vec2(left,top+height-py),glm::vec2(width,py),glm::vec4(1));
                renderer.draw_rect(glm::vec2(left,top),glm::vec2(px,height),glm::vec4(1));
                renderer.draw_rect(glm::vec2(left+width-px,top),glm::vec2(px,height),glm::vec4(1));
            }
            renderer.reset_clip();
        }
        if(!message.empty()) renderer.draw_text(message,0.05f,0.46f,0.03f,glm::vec4(1,0.75f,0.65f,1));
        if(busy()) renderer.draw_text(operation.valid()?"Saving…":"Loading…",0.78f,0.04f,0.028f);
        const auto hints=cropping ? "Arrows move crop · +/- or wheel resize · Enter saves a copy · Escape cancels"
            : "Left/Right photo · Up/Down zoom · Drag to pan · R rotate · Space slideshow · I information · Tab options";
        renderer.draw_text(hints,0.045f,0.87f,0.022f,glm::vec4(0.8f,0.83f,0.88f,1));
        if(cropping) {
            render_controller_buttons(xmb,renderer,0.5f,0.94f,std::array{
                std::pair{action::ok,std::string_view{"Save crop"}}, std::pair{action::extra,std::string_view{"Resize"}}, std::pair{action::cancel,std::string_view{"Cancel"}}});
        } else {
            render_controller_buttons(xmb,renderer,0.5f,0.94f,std::array{
                std::pair{action::left,std::string_view{"Previous"}}, std::pair{action::right,std::string_view{"Next"}},
                std::pair{action::ok,slideshow?std::string_view{"Pause slideshow"}:std::string_view{"Slideshow"}},
                std::pair{action::options,std::string_view{"Options"}},std::pair{action::cancel,std::string_view{"Close"}}});
        }
    }
    result on_action(action action) override {
        if(cropping) {
            if(action==action::cancel) { cropping=false; return result::success; }
            if(action==action::ok) { cropping=false; start_save(crop_region); return result::success; }
            if(action==action::extra) { resize_crop(0.9); return result::success; }
            if(action==action::left) crop_region.x=std::max(0.0,crop_region.x-0.025);
            else if(action==action::right) crop_region.x=std::min(1.0-crop_region.width,crop_region.x+0.025);
            else if(action==action::up) crop_region.y=std::max(0.0,crop_region.y-0.025);
            else if(action==action::down) crop_region.y=std::min(1.0-crop_region.height,crop_region.y+0.025);
            return result::success;
        }
        if(action==action::cancel) return result::close;
        if(action==action::left) { navigate(-1); return result::success; }
        if(action==action::right) { navigate(1); return result::success; }
        if(action==action::ok) { slideshow=!slideshow; last_slide=clock::now(); return result::success; }
        if(action==action::options) { options(); return result::success; }
        auto result=base_viewer::on_action(action);
        zoom=std::clamp(zoom,0.1f,16.0f);
        return result;
    }
    result on_joystick(unsigned int index,float x,float y) override {
        if(cropping) { crop_region.x=std::clamp(crop_region.x+x*0.01,0.0,1.0-crop_region.width); crop_region.y=std::clamp(crop_region.y+y*0.01,0.0,1.0-crop_region.height); return result::success; }
        return base_viewer::on_joystick(index,x,y);
    }
    result on_mouse_move(float x,float y) override {
        if(dragging) { offset += glm::vec2((x-last_mouse_x)*view_aspect,y-last_mouse_y); }
        last_mouse_x=x; last_mouse_y=y;
        return result::success;
    }
    result on_event(const event& event) override {
        if(auto* key=event.get<events::key_down>()) {
            switch(static_cast<sdl::Scancode>(key->keycode)) {
                case sdl::Scancode::SDL_SCANCODE_R: pending_action=command::rotate_right; return result::success;
                case sdl::Scancode::SDL_SCANCODE_SPACE: return on_action(action::ok);
                case sdl::Scancode::SDL_SCANCODE_I: pending_action=command::information; return result::success;
                case sdl::Scancode::SDL_SCANCODE_W: pending_action=command::wallpaper; return result::success;
                case sdl::Scancode::SDL_SCANCODE_S: pending_action=command::save; return result::success;
                case sdl::Scancode::SDL_SCANCODE_DELETE: pending_action=command::trash; return result::success;
                case sdl::Scancode::SDL_SCANCODE_EQUALS: if(cropping) resize_crop(0.9); else zoom=std::min(16.0f,zoom*1.1f); return result::success;
                case sdl::Scancode::SDL_SCANCODE_MINUS: if(cropping) resize_crop(1.1); else zoom=std::max(0.1f,zoom/1.1f); return result::success;
                default: break;
            }
        }
        if(auto* button=event.get<events::controller_button_down>()) {
            if(button->button==events::logical_controller_button::leftshoulder) { if(cropping) resize_crop(1.1); else navigate(-1); return result::success; }
            if(button->button==events::logical_controller_button::rightshoulder) { if(cropping) resize_crop(0.9); else navigate(1); return result::success; }
        }
        if(auto* axis=event.get<events::joystick_axis>()) return on_joystick(static_cast<unsigned int>(axis->index),axis->x,axis->y);
        if(auto* mouse=event.get<events::mouse_move>()) return on_mouse_move(mouse->x,mouse->y);
        if(auto* scroll=event.get<events::mouse_scroll>()) { if(cropping) resize_crop(scroll->x>0?0.9:1.1); else zoom=std::clamp(zoom*(scroll->x>0?1.1f:1.0f/1.1f),0.1f,16.0f); return result::success; }
        if(auto* mouse=event.get<events::mouse_button_down>(); mouse && mouse->button==events::logical_mouse_button::left) { dragging=true; return result::success; }
        if(event.is<events::mouse_button_up>()) { dragging=false; return result::success; }
        return on_action(event.action);
    }
    [[nodiscard]] bool is_opaque() const override { return true; }
    [[nodiscard]] bool enable_cursor() const override { return true; }
private:
    using clock=std::chrono::steady_clock;
    enum class command { rotate_right,rotate_left,information,wallpaper,save,crop_square,crop_landscape,crop_classic,trash,interval };
    enum class operation_kind { saved,wallpaper,trash };
    struct Loaded { photo::Bitmap image; photo::Metadata info; std::vector<std::filesystem::path> images; std::uint64_t generation{}; };
    struct Operation { operation_kind kind; std::filesystem::path path; };
    template<class T> static bool ready(std::future<T>& future) { return future.wait_for(std::chrono::seconds(0))==std::future_status::ready; }
    void wait_gpu() { if(xmb) xmb->get_window()->waitIdle(); else if(loader) loader->waitIdle(); }
    bool busy() const { return decode.valid() || upload.valid() || operation.valid() || load_requested; }
    void request_load() {
        if(!loader || path.empty()) return;
        ++generation; load_requested=true; error_state=false; message.clear();
        zoom=1; offset=glm::vec2(0); move_delta_pos=glm::vec2(0); cropping=false;
        if(!decode.valid() && !upload.valid()) begin_load();
    }
    void begin_load() {
        load_requested=false;
        const auto selected=path; const int rotation=turns; const auto version=generation; const bool scan=images.empty();
        decode=std::async(std::launch::async,[selected,rotation,version,scan]() {
            Loaded result;
            auto loaded=photo::load_photo(selected);
            result.info=std::move(loaded.metadata);
            result.image=photo::rotate(std::move(loaded.image),rotation);
            if(scan) result.images=photo::gallery(selected);
            result.generation=version;
            return result;
        });
    }
    void navigate(int delta) {
        if(images.size()<2 || operation.valid()) return;
        selected_index=(selected_index+images.size()+delta)%images.size(); path=images[selected_index]; turns=0; last_slide=clock::now(); request_load();
    }
    std::array<float,4> layout(float aspect) const {
        const float picture=static_cast<float>(image_width)/std::max(1u,image_height);
        float width=0.91f, height=width*aspect/picture;
        if(height>0.73f) { height=0.73f; width=height*picture/aspect; }
        const float scale=cropping?1.0f:zoom;
        const glm::vec2 pan=cropping?glm::vec2(0):offset;
        return {0.5f-width*scale*0.5f+pan.x/aspect,0.47f-height*scale*0.5f+pan.y,width*scale,height*scale};
    }
    void resize_crop(double factor) {
        factor=std::clamp(factor,std::max(0.05/crop_region.width,0.05/crop_region.height),std::min(1.0/crop_region.width,1.0/crop_region.height));
        const double width=crop_region.width*factor;
        const double height=crop_region.height*factor;
        crop_region.x=std::clamp(crop_region.x+(crop_region.width-width)*0.5,0.0,1.0-width);
        crop_region.y=std::clamp(crop_region.y+(crop_region.height-height)*0.5,0.0,1.0-height);
        crop_region.width=width; crop_region.height=height;
    }
    void options() {
        if(!xmb || modal_open) return;
        if(path.empty()) return;
        modal_open=true;
        xmb->emplace_overlay<choice_overlay>(std::vector<std::string>{
            "Rotate clockwise","Rotate counterclockwise","Photo information","Set as wallpaper","Save rotated copy",
            "Crop square…","Crop 16:9…","Crop 4:3…","Move to OpenXMB Trash…","Slideshow interval…"},0,
            [this](unsigned int selected) { modal_open=false; if(selected<=9) pending_action=static_cast<command>(selected); },
            [this]() { modal_open=false; });
    }
    void perform(command selected) {
        if(!xmb || path.empty() || operation.valid()) return;
        if((decode.valid() || upload.valid()) && selected!=command::rotate_right && selected!=command::rotate_left && selected!=command::interval) {
            message="Wait for the photo to finish loading";
            return;
        }
        if(selected==command::rotate_right || selected==command::rotate_left) { turns=(turns+(selected==command::rotate_right?1:3))%4; request_load(); }
        else if(selected==command::information) notify("Photo information",photo::describe(info));
        else if(selected==command::save) start_save(std::nullopt);
        else if(selected==command::wallpaper) {
            if(turns==0 && info.orientation==1) apply_wallpaper(path);
            else { const auto source=path; const int rotation=turns; operation=std::async(std::launch::async,[source,rotation]() { return Operation{operation_kind::wallpaper,photo::save_copy(source,rotation)}; }); }
        } else if(selected==command::crop_square || selected==command::crop_landscape || selected==command::crop_classic) {
            if(!texture || busy()) return;
            cropping=true; slideshow=false;
            crop_region=photo::centered_crop(image_width,image_height,selected==command::crop_square?1.0:selected==command::crop_landscape?16.0/9.0:4.0/3.0);
        } else if(selected==command::interval) {
            modal_open=true;
            xmb->emplace_overlay<choice_overlay>(std::vector<std::string>{"3 seconds","5 seconds","10 seconds","15 seconds"},1,
                [this](unsigned int selected) { constexpr std::array values{3,5,10,15}; slide_interval=std::chrono::seconds(values.at(selected)); modal_open=false; last_slide=clock::now(); },
                [this]() { modal_open=false; });
        } else if(selected==command::trash) {
            modal_open=true; slideshow=false; const auto source=path;
            xmb->emplace_overlay<message_overlay>("Move photo to OpenXMB Trash?",source.filename().string()+" will be moved to a recovery folder. It will not be permanently deleted.",
                std::vector<std::string>{"Cancel","Move to Trash"},
                [this,source](unsigned int selected) { modal_open=false; if(selected==1) operation=std::async(std::launch::async,[source]() { return Operation{operation_kind::trash,photo::move_to_trash(source)}; }); },
                true,[this]() { modal_open=false; });
        }
    }
    void start_save(std::optional<photo::Crop> region) {
        if(operation.valid() || path.empty()) return;
        const auto source=path; const int rotation=turns;
        operation=std::async(std::launch::async,[source,rotation,region]() { return Operation{operation_kind::saved,photo::save_copy(source,rotation,region)}; });
    }
    void apply_wallpaper(const std::filesystem::path& selected) {
        config::CONFIG.setBackgroundImage(selected.string()); config::CONFIG.setBackgroundType(std::string_view("image")); config::CONFIG.save_config();
        notify("Wallpaper updated",selected.filename().string());
    }
    void notify(std::string title,std::string text) {
        modal_open=true;
        xmb->emplace_overlay<message_overlay>(std::move(title),std::move(text),std::vector<std::string>{"OK"},
            [this](unsigned int) { modal_open=false; },true,[this]() { modal_open=false; });
    }
    std::filesystem::path path;
    dreamrender::resource_loader* loader{};
    shell* xmb{};
    std::shared_ptr<dreamrender::texture> texture,incoming;
    std::future<Loaded> decode;
    std::future<void> upload;
    std::future<Operation> operation;
    std::vector<std::filesystem::path> images;
    std::size_t selected_index{};
    std::uint64_t generation{},upload_generation{};
    int turns{};
    photo::Metadata info;
    std::optional<command> pending_action;
    photo::Crop crop_region;
    std::string message;
    bool load_requested{},modal_open{},cropping{},slideshow{},error_state{},close_pending{},dragging{};
    float last_mouse_x{},last_mouse_y{},view_aspect{1.0f};
    clock::time_point last_tick=clock::now(),last_slide=clock::now();
    std::chrono::seconds slide_interval{5};
};

namespace {
const inline register_program<image_viewer> image_viewer_program{
    "image_viewer",
    {"image/bmp","image/gif","image/jpeg","image/vnd.zbrush.pcx","image/png","image/x-portable-bitmap","image/x-portable-graymap","image/x-portable-pixmap","image/x-portable-anymap","image/svg+xml","image/x-targa","image/x-tga","image/tiff","image/webp","image/x-xcf","image/x-xpixmap"},
    {".bmp",".gif",".jpg",".jpeg",".pcx",".png",".pbm",".pgm",".ppm",".pnm",".svg",".tga",".tiff",".tif",".webp",".xcf",".xpm"}};
}
}
