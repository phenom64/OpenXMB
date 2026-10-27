/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025-2026 Syndromatic Ltd. All rights reserved
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

#include "../app/components/dialog_input.hpp"
#include <openxmb/photos/photo_library.hpp>
#include <openxmb/integrations/desktop.hpp>
#include <fstream>
#include <sstream>
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

import auroreengine;
import glm;
import sdl2;
import vulkan_hpp;
import openxmb.config;
import openxmb.utils;
import :component;
import :programs;
import :base_viewer;
import :xmb_presentation;
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

    image_viewer(std::filesystem::path selected, auroreengine::resource_loader& resources)
        : path(std::filesystem::absolute(std::move(selected)).lexically_normal()), loader(&resources) {
        image_width = image_height = 1;
        slide_interval=std::chrono::seconds(static_cast<int>(config::CONFIG.number("photo.duration",5)));
        effect_fade=config::CONFIG.preference("photo.transition","Fade")=="Fade";
        if(path.extension()==".xmbphotos") {
            std::ifstream stream(path);std::string line;
            while(std::getline(stream,line)) if(!line.empty()&&line[0]!='#') {std::filesystem::path entry(line);if(entry.is_relative()) entry=path.parent_path()/entry;if(photo::supported_image(entry)&&std::filesystem::is_regular_file(entry)) images.push_back(entry);}
            if(!images.empty()) path=images.front();
        }
        for(const auto* name:{"vp/screen_mode","pv/change_effect","pv/trimming","system/mp_addplaylist","pv/print","pv/set_as_wallpaper","system/mp_delete","system/mp_display","pv/zoom_in","pv/zoom_out","pv/rotate_left","pv/rotate_right","pv/up","pv/down","pv/left","pv/right","system/mp_prev","system/mp_next","system/mp_play"}) icons.load(resources,name);
        request_load();
    }
    image_viewer(std::shared_ptr<auroreengine::texture> image) : texture(std::move(image)) {
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
                    incoming = std::make_shared<auroreengine::texture>(loader->getDevice(), loader->getAllocator(), pixels->width, pixels->height);
                    upload_generation=generation;
                    upload = loader->loadTexture(incoming.get(), auroreengine::LoaderFunction{
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
                    previous_image=std::move(texture);transition_started=now;
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
        if(slideshow && !modal_open && !cropping && !busy() && images.size()>1 && now-last_slide>=slide_interval) {
            if(!config::CONFIG.enabled("photo.repeat")&&selected_index+1>=images.size())slideshow=false;
            else if(config::CONFIG.enabled("photo.shuffle",false))navigate(1+static_cast<int>(now.time_since_epoch().count()%std::max(std::size_t{1},images.size()-1)));
            else navigate(1);
        }
        if(previous_image&&now-transition_started>std::chrono::milliseconds(500)) {wait_gpu();previous_image.reset();}
        if(close_pending && !modal_open) return result::close;
        return result::success;
    }

    void render(auroreengine::gui_renderer& renderer, shell*) override {
        view_aspect=static_cast<float>(renderer.aspect_ratio);
        renderer.draw_rect(glm::vec2(0),glm::vec2(1),glm::vec4(0,0,0,1));
        app::xmb_canvas canvas(renderer);
        if(texture && texture->loaded.load(std::memory_order_acquire) && !error_state) {
            const auto geometry=layout(static_cast<float>(renderer.aspect_ratio));
            renderer.set_clip(0,0,1,1);
            const float blend=effect_fade?std::clamp(std::chrono::duration<float>(clock::now()-transition_started).count()/0.5f,0.0f,1.0f):1.0f;
            if(previous_image&&blend<1) renderer.draw_image(*previous_image,geometry[0],geometry[1],geometry[2]*renderer.aspect_ratio,geometry[3]);
            renderer.draw_image(*texture,geometry[0],geometry[1],geometry[2]*renderer.aspect_ratio,geometry[3],{1,1,1,blend});
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
        if(display_on) {
            canvas.rect(0,0,1920,130,{0,0,0,0.64f});
            canvas.text(path.stem().string(),132,114,27,{0.83f,0.68f,0.36f,1});
            canvas.text("("+std::to_string(selected_index+1)+"/"+std::to_string(images.size())+")  "+(info.captured.empty()?info.modified:info.captured),1470,114,25,{0.83f,0.68f,0.36f,1});
        }
        if(controls_on) draw_controls(canvas);
        if(information_on) {
            canvas.rect(1106,231,643,633,{0,0,0,0.78f});
            std::istringstream lines(photo::describe(info));std::string line;float y=275;
            while(std::getline(lines,line)) {canvas.text(line,1140,y,23);y+=40;}
        }
        if(!message.empty()) canvas.text(message,100,922,24,{1,0.84f,0.65f,1});
        if(busy()) canvas.text(operation.valid()?"Saving…":"Loading…",1600,107,24);
        if(cropping) {
            canvas.text("Trimming",960,170,30,{1,1,1,1},0,true);
            render_controller_buttons(xmb,renderer,0.5f,0.94f,std::array{
                std::pair{action::ok,std::string_view{"Save crop"}},std::pair{action::extra,std::string_view{"Resize"}},std::pair{action::cancel,std::string_view{"Cancel"}}});
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
        if(action==action::options) { controls_on=!controls_on;return result::success; }
        if(controls_on) {
            constexpr std::array<int,3> counts{8,8,3};
            if(action==action::cancel) {controls_on=false;return result::success;}
            if(action==action::left) control_column=(control_column+counts[control_row]-1)%counts[control_row];
            else if(action==action::right) control_column=(control_column+1)%counts[control_row];
            else if(action==action::up||action==action::down) {control_row=(control_row+(action==action::up?2:1))%3;control_column=std::min(control_column,counts[control_row]-1);}
            else if(action==action::ok) activate_control();
            return result::success;
        }
        if(action==action::cancel) return result::close;
        if(action==action::left) { navigate(-1); return result::success; }
        if(action==action::right) { navigate(1); return result::success; }
        if(action==action::ok) { slideshow=!slideshow; last_slide=clock::now(); return result::success; }

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
        return app::dialog_input::dispatch(event.action,
            event.is<events::key_down>()||event.is<events::controller_button_down>(),
            [this](action mapped){return on_action(mapped);},[&]() -> result {
        if(auto* key=event.get<events::key_down>()) {
            switch(static_cast<sdl::Scancode>(key->keycode)) {
                case sdl::Scancode::SDL_SCANCODE_R: pending_action=command::rotate_right; return result::success;
                case sdl::Scancode::SDL_SCANCODE_SPACE: return on_action(action::ok);
                case sdl::Scancode::SDL_SCANCODE_D: display_on=!display_on; return result::success;
                case sdl::Scancode::SDL_SCANCODE_I: information_on=!information_on; return result::success;
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
        });
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
        float width=1.0f, height=width*aspect/picture;
        if(height>1.0f) { height=1.0f; width=height*picture/aspect; }
        if(!cropping&&config::CONFIG.preference("photo.fit","Fit")=="Fill") {const float factor=std::max(1.0F/width,1.0F/height);width*=factor;height*=factor;}
        const float scale=cropping?1.0f:zoom;
        const glm::vec2 pan=cropping?glm::vec2(0):offset;
        return {0.5f-width*scale*0.5f+pan.x/aspect,0.5f-height*scale*0.5f+pan.y,width*scale,height*scale};
    }
    void resize_crop(double factor) {
        factor=std::clamp(factor,std::max(0.05/crop_region.width,0.05/crop_region.height),std::min(1.0/crop_region.width,1.0/crop_region.height));
        const double width=crop_region.width*factor;
        const double height=crop_region.height*factor;
        crop_region.x=std::clamp(crop_region.x+(crop_region.width-width)*0.5,0.0,1.0-width);
        crop_region.y=std::clamp(crop_region.y+(crop_region.height-height)*0.5,0.0,1.0-height);
        crop_region.width=width; crop_region.height=height;
    }
    void draw_controls(const app::xmb_canvas& c) {
        static constexpr std::array<std::string_view,19> names{"vp/screen_mode","pv/change_effect","pv/trimming","system/mp_addplaylist","pv/print","pv/set_as_wallpaper","system/mp_delete","system/mp_display","pv/zoom_in","pv/zoom_out","pv/rotate_left","pv/rotate_right","pv/up","pv/down","pv/left","pv/right","system/mp_prev","system/mp_next","system/mp_play"};
        static constexpr std::array<std::string_view,19> labels{"Display Mode","Change Effect","Trimming","Add to Playlist","Print","Set as Wallpaper","Delete","Display","Zoom In","Zoom Out","Rotate Left","Rotate Right","Up","Down","Left","Right","Previous","Next","Slideshow"};
        const int selected=control_row<2?control_row*8+control_column:16+control_column;
        for(int row=0;row<3;++row) {const int count=row==2?3:8,first=row*8;
            for(int column=0;column<count;++column) {int index=first+column;bool focus=selected==index;
                icons.draw(c,names[index],514+(column-(count-1)*0.5f)*64,440+row*65,72,focus?1:0.85f,focus?1.3f:1);
            }
        }
        c.text(labels[selected],514,648,26,{0.83f,0.68f,0.36f,1},app::xmb_canvas::pulse(),true);
    }
    void activate_control() {
        const int selected=control_row<2?control_row*8+control_column:16+control_column;
        switch(selected) {
        case 0: zoom=zoom==1?1.3f:1.0f;offset={};break;
        case 1: effect_fade=!effect_fade;message=effect_fade?"Fade":"Normal";break;
        case 2: controls_on=false;pending_action=command::crop_classic;break;
        case 3: {
            const auto destination=openxmb::integrations::user_data_directory()/"Photo Playlists";
            std::filesystem::create_directories(destination);
            const auto playlist=destination/(path.stem().string()+".xmbphotos");
            std::ofstream file(playlist);file<<"#OPENXMB-PHOTOS\n";for(const auto& image:images) file<<image.string()<<'\n';
            message=file?"Slideshow saved: "+playlist.filename().string():"Could not save slideshow";break;
        }
        case 4: {
            const auto program=openxmb::integrations::find_executable("lp");
            if(!program) {message="No print service is available";break;}
            const auto result=openxmb::integrations::launch_process({program->string(),"--",path.string()});message=result?"Photo sent to print service":result.message;break;
        }
        case 5: pending_action=command::wallpaper;break;case 6:pending_action=command::trash;break;
        case 7: display_on=!display_on;break;case 8:zoom=std::min(16.0f,zoom*1.1f);break;case 9:zoom=std::max(0.1f,zoom/1.1f);break;
        case 10:pending_action=command::rotate_left;break;case 11:pending_action=command::rotate_right;break;
        case 12:offset.y-=0.03f;break;case 13:offset.y+=0.03f;break;case 14:offset.x-=0.03f;break;case 15:offset.x+=0.03f;break;
        case 16:navigate(-1);break;case 17:navigate(1);break;case 18:slideshow=!slideshow;last_slide=clock::now();break;
        }
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
    auroreengine::resource_loader* loader{};
    shell* xmb{};
    std::shared_ptr<auroreengine::texture> texture,incoming,previous_image;
    app::media_icons icons;
    bool controls_on{},display_on{true},information_on{},effect_fade{true};
    int control_row{},control_column{};
    clock::time_point transition_started=clock::now();
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
    {".bmp",".gif",".jpg",".jpeg",".pcx",".png",".pbm",".pgm",".ppm",".pnm",".svg",".tga",".tiff",".tif",".webp",".xcf",".xpm",".xmbphotos"}};
}
}
