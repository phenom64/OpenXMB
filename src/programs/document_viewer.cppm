/* OpenXMB document viewer. SPDX-License-Identifier: GPL-3.0-or-later */
module;
#include <openxmb/documents/document.hpp>
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
#include <stop_token>
#include <string>
#include <string_view>

export module openxmb.app:document_viewer;
import dreamrender;
import glm;
import sdl2;
import openxmb.utils;
import :component;
import :programs;
import :base_viewer;
import :choice_overlay;
import :message_overlay;
import openxmb.app;

namespace programs {
using namespace app;
namespace documents=openxmb::documents;

export class document_viewer : private base_viewer,public component,
    public action_receiver,public event_receiver,public joystick_receiver,
    public mouse_receiver {
public:
    float typography_scale() const override { return 2.5F; }

    document_viewer(std::filesystem::path path,dreamrender::resource_loader& loader)
        : path(std::filesystem::absolute(std::move(path))),loader(loader) {
        image_width=image_height=1;
        request(0);
    }
    ~document_viewer() override {
        cancellation.request_stop();
        if(texture || incoming) { try { wait_gpu(); } catch(...) {} }
    }
    result tick(shell* shell) override {
        xmb=shell;
        const auto now=clock::now();
        offset+=move_delta_pos*std::clamp(std::chrono::duration<float>(now-last_tick).count(),0.0f,0.1f)*60.0f;
        offset=glm::clamp(offset,glm::vec2(-zoom),glm::vec2(zoom)); last_tick=now;
        if(pending) { const auto action=*pending; pending.reset(); perform(action); }
        if(decoding.valid() && ready(decoding)) {
            try {
                auto loaded=decoding.get();
                document=loaded.document;
                if(loaded.generation==generation) {
                    auto pixels=std::make_shared<openxmb::photos::Bitmap>(std::move(loaded.image));
                    incoming=std::make_shared<dreamrender::texture>(loader.getDevice(),loader.getAllocator(),pixels->width,pixels->height);
                    upload_generation=generation;
                    upload=loader.loadTexture(incoming.get(),dreamrender::LoaderFunction{[pixels](std::uint8_t* output,std::size_t capacity) {
                        if(pixels->rgba.size()>capacity) throw std::runtime_error("Document page exceeds texture upload limit");
                        std::memcpy(output,pixels->rgba.data(),pixels->rgba.size());
                    }});
                }
            } catch(const std::exception& error) { message=error.what(); error_state=true; }
            if(requested && !upload.valid()) begin_load();
        }
        if(upload.valid() && ready(upload)) {
            try {
                upload.get();
                if(upload_generation==generation) {
                    if(texture) wait_gpu();
                    texture=std::move(incoming); image_width=texture->width; image_height=texture->height;
                    error_state=false; message.clear();
                }
            } catch(const std::exception& error) { message=error.what(); error_state=true; }
            if(requested && !decoding.valid()) begin_load();
        }
        if(printing.valid() && ready(printing)) {
            try { printing.get(); notify("Print job submitted","The document was accepted by the configured print command."); }
            catch(const std::exception& error) { notify("Printing failed",error.what()); }
        }
        return result::success;
    }
    void render(dreamrender::gui_renderer& renderer,shell*) override {
        view_aspect=static_cast<float>(renderer.aspect_ratio);
        renderer.draw_rect(glm::vec2(0),glm::vec2(1),glm::vec4(0.02f,0.025f,0.035f,1));
        std::string title=path.filename().string();
        if(document) title+="   "+std::to_string(page+1)+" / "+std::to_string(document->pages);
        renderer.draw_text(title,0.04f,0.025f,0.032f);
        if(texture && texture->loaded.load(std::memory_order_acquire) && !error_state) {
            const float aspect=static_cast<float>(image_width)/std::max(1u,image_height);
            float width=0.91f,height=width*renderer.aspect_ratio/aspect;
            if(height>0.77f) { height=0.77f; width=height*aspect/renderer.aspect_ratio; }
            renderer.set_clip(0.03f,0.085f,0.94f,0.79f);
            renderer.draw_image(*texture,0.5f-width*zoom*0.5f+offset.x/renderer.aspect_ratio,
                0.48f-height*zoom*0.5f+offset.y,width*zoom*renderer.aspect_ratio,height*zoom);
            renderer.reset_clip();
        }
        if(!message.empty()) renderer.draw_text(message,0.05f,0.4f,0.03f,glm::vec4(1,0.75f,0.7f,1));
        if(decoding.valid() || upload.valid()) renderer.draw_text("Loading page…",0.74f,0.03f,0.026f);
        if(printing.valid()) renderer.draw_text("Submitting print job…",0.66f,0.03f,0.026f);
        renderer.draw_text("Left/Right page · Home/End first/last · Up/Down or wheel zoom · Drag to pan · P print · Tab options",0.04f,0.89f,0.022f,glm::vec4(0.8f,0.83f,0.88f,1));
        render_controller_buttons(xmb,renderer,0.5f,0.95f,std::array{
            std::pair{action::left,std::string_view{"Previous page"}},std::pair{action::right,std::string_view{"Next page"}},
            std::pair{action::extra,std::string_view{"Fit page"}},std::pair{action::options,std::string_view{"Options"}},std::pair{action::cancel,std::string_view{"Close"}}});
    }
    result on_action(action action) override {
        if(action==action::cancel) { cancellation.request_stop(); return result::close; }
        if(action==action::ok) { navigate(1); return result::success; }
        if(action==action::left) { navigate(-1); return result::success; }
        if(action==action::right) { navigate(1); return result::success; }
        if(action==action::options) { options(); return result::success; }
        auto handled=base_viewer::on_action(action); zoom=std::clamp(zoom,0.1f,16.0f); return handled==result::unsupported?result::success:handled;
    }
    result on_joystick(unsigned int index,float x,float y) override { return base_viewer::on_joystick(index,x,y); }
    result on_mouse_move(float x,float y) override {
        if(dragging) offset+=glm::vec2((x-last_x)*view_aspect,y-last_y);
        last_x=x;last_y=y; return result::success;
    }
    result on_event(const event& event) override {
        if(auto* key=event.get<events::key_down>()) {
            switch(static_cast<sdl::Scancode>(key->keycode)) {
                case sdl::Scancode::SDL_SCANCODE_HOME: if(document) request(0); return result::success;
                case sdl::Scancode::SDL_SCANCODE_END: if(document) request(document->pages-1); return result::success;
                case sdl::Scancode::SDL_SCANCODE_PAGEUP: navigate(-1); return result::success;
                case sdl::Scancode::SDL_SCANCODE_PAGEDOWN: navigate(1); return result::success;
                case sdl::Scancode::SDL_SCANCODE_P: pending=command::print_page; return result::success;
                case sdl::Scancode::SDL_SCANCODE_I: pending=command::information; return result::success;
                case sdl::Scancode::SDL_SCANCODE_F: return on_action(action::extra);
                default: break;
            }
        }
        if(auto* button=event.get<events::controller_button_down>()) {
            if(button->button==events::logical_controller_button::leftshoulder) { navigate(-1); return result::success; }
            if(button->button==events::logical_controller_button::rightshoulder) { navigate(1); return result::success; }
        }
        if(auto* axis=event.get<events::joystick_axis>()) return on_joystick(static_cast<unsigned int>(axis->index),axis->x,axis->y);
        if(auto* mouse=event.get<events::mouse_move>()) return on_mouse_move(mouse->x,mouse->y);
        if(auto* scroll=event.get<events::mouse_scroll>()) { zoom=std::clamp(zoom*(scroll->x>0?1.1f:1.0f/1.1f),0.1f,16.0f); return result::success; }
        if(auto* mouse=event.get<events::mouse_button_down>();mouse && mouse->button==events::logical_mouse_button::left) { dragging=true; return result::success; }
        if(event.is<events::mouse_button_up>()) { dragging=false; return result::success; }
        return on_action(event.action);
    }
    [[nodiscard]] bool is_opaque() const override { return true; }
    [[nodiscard]] bool enable_cursor() const override { return true; }
private:
    using clock=std::chrono::steady_clock;
    enum class command { previous,next,first,last,information,print_page,print_all };
    struct Loaded { std::shared_ptr<documents::Document> document; openxmb::photos::Bitmap image; std::uint64_t generation{}; };
    template<class T> static bool ready(std::future<T>& future) { return future.wait_for(std::chrono::seconds(0))==std::future_status::ready; }
    void wait_gpu() { if(xmb) xmb->get_window()->waitIdle(); else loader.waitIdle(); }
    void request(std::size_t selected) {
        if(printing.valid()) return;
        page=selected; ++generation; requested=true; error_state=false; message.clear(); zoom=1;offset=glm::vec2(0);move_delta_pos=glm::vec2(0);
        if(!decoding.valid() && !upload.valid()) begin_load();
    }
    void begin_load() {
        requested=false;
        const auto selected=page,version=generation; const auto source=path; auto existing=document; const auto stop=cancellation.get_token();
        decoding=std::async(std::launch::async,[source,selected,version,existing,stop]() {
            auto document=existing?existing:documents::open(source,stop);
            auto raster=documents::render_page(*document,selected,stop);
            return Loaded{document,openxmb::photos::load_bitmap(raster),version};
        });
    }
    void navigate(int direction) {
        if(!document || printing.valid()) return;
        if(direction<0 && page>0) request(page-1);
        if(direction>0 && page+1<document->pages) request(page+1);
    }
    void options() {
        if(!xmb || modal_open || !document) return;
        modal_open=true;
        std::vector<std::string> choices{"Previous page","Next page","First page","Last page","Document information","Print current page…"};
        if(document->kind==documents::Kind::pdf) choices.push_back("Print entire PDF…");
        xmb->emplace_overlay<choice_overlay>(std::move(choices),0,[this](unsigned int selected) { modal_open=false; if(selected<=6) pending=static_cast<command>(selected); },[this]() { modal_open=false; });
    }
    void perform(command action) {
        if(!document || !xmb) return;
        if(action==command::previous) navigate(-1);
        else if(action==command::next) navigate(1);
        else if(action==command::first) request(0);
        else if(action==command::last) request(document->pages-1);
        else if(action==command::information) notify("Document information",documents::information(*document));
        else if(action==command::print_page || action==command::print_all) {
            if(decoding.valid() || upload.valid() || printing.valid()) return;
            if(!documents::printing_available()) { notify("Printing unavailable","Install CUPS client tools (lp or lpr), configure a printer, and add the tools to PATH."); return; }
            modal_open=true; const bool all=action==command::print_all;
            auto selected=all?std::optional<std::size_t>{}:std::optional<std::size_t>{page};
            xmb->emplace_overlay<message_overlay>("Print document?",all?"Submit the entire PDF to the default printer?":"Submit page "+std::to_string(page+1)+" to the default printer?",
                std::vector<std::string>{"Cancel","Print"},[this,selected](unsigned int choice) {
                    modal_open=false;
                    if(choice==1) { auto current=document; auto stop=cancellation.get_token(); printing=std::async(std::launch::async,[current,selected,stop]() { documents::print(*current,selected,stop); }); }
                },true,[this]() { modal_open=false; });
        }
    }
    void notify(std::string title,std::string body) {
        modal_open=true;
        xmb->emplace_overlay<message_overlay>(std::move(title),std::move(body),std::vector<std::string>{"OK"},[this](unsigned int) { modal_open=false; },true,[this]() { modal_open=false; });
    }
    std::filesystem::path path;
    dreamrender::resource_loader& loader;
    shell* xmb{};
    std::shared_ptr<documents::Document> document;
    std::shared_ptr<dreamrender::texture> texture,incoming;
    std::future<Loaded> decoding;
    std::future<void> upload,printing;
    std::stop_source cancellation;
    std::optional<command> pending;
    std::size_t page{};
    std::uint64_t generation{},upload_generation{};
    std::string message;
    bool requested{},error_state{},modal_open{},dragging{};
    float last_x{},last_y{},view_aspect{1.0f};
    clock::time_point last_tick=clock::now();
};
namespace {
const inline register_program<document_viewer> document_viewer_program{
    "document_viewer",{"application/pdf","application/vnd.comicbook+zip","application/x-cbz","application/x-cbt","application/vnd.comicbook+tar"},{".pdf",".cbz",".cbt"}};
}
}
