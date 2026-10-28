module;
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "openxmb/xmb/background.hpp"
#include "../localization/ui_literal_text.hpp"
export module openxmb.app:xmb_presentation;
import auroreengine;
import glm;
import openxmb.config;
import openxmb.utils;
import vulkan_hpp;
export namespace app {
// One contained design space for every XMB scene. Text uses baseline anchors.
struct xmb_canvas {
    auroreengine::gui_renderer& renderer;
    float scale, ox, oy;
    explicit xmb_canvas(auroreengine::gui_renderer& r):renderer(r) {
        scale=std::min(r.frame_size.width/1920.0f,r.frame_size.height/1080.0f);
        ox=(r.frame_size.width-1920.0f*scale)*0.5f;oy=(r.frame_size.height-1080.0f*scale)*0.5f;
    }
    void text(std::string_view text,float x,float baseline,float size=28,glm::vec4 color=glm::vec4(1),float glow=0,bool centered=false) const {
        renderer.draw_text_pixels(text,ox+x*scale,oy+baseline*scale,size*scale*config::CONFIG.textScale,color,0.7f,glow,centered);
    }
    template<class Character, std::size_t N>
        requires std::is_same_v<Character, const char>
    void text(Character (&literal)[N],float x,float baseline,float size=28,glm::vec4 color=glm::vec4(1),float glow=0,bool centered=false) const {
        const auto localized=detail::localize_ui_literal(literal);
        text(std::string_view(localized),x,baseline,size,color,glow,centered);
    }
    void rect(float x,float y,float w,float h,glm::vec4 color,float radius=0) const {
        auroreengine::simple_params parameters{};
        if(radius>0&&h>0) {parameters.aspect_ratio=w/h;parameters.border_radius.fill(std::min(radius/h,0.5f));}
        renderer.draw_rect({(ox+x*scale)/renderer.frame_size.width,(oy+y*scale)/renderer.frame_size.height},
            {w*scale/renderer.frame_size.width,h*scale/renderer.frame_size.height},color,parameters);
    }
    void gradient(float x,float y,float w,float h,glm::vec4 top,glm::vec4 bottom) const {
        using V=auroreengine::simple_renderer::vertex_data;
        const float left=(ox+x*scale)/renderer.frame_size.width,right=(ox+(x+w)*scale)/renderer.frame_size.width;
        const float upper=(oy+y*scale)/renderer.frame_size.height,lower=(oy+(y+h)*scale)/renderer.frame_size.height;
        const std::array<V,4> vertices{V{{left,upper},top,{0,0}},V{{right,upper},top,{1,0}},V{{left,lower},bottom,{0,1}},V{{right,lower},bottom,{1,1}}};
        renderer.draw_quad(vertices);
    }
    void image(const auroreengine::texture& t,float x,float y,float w,float h,glm::vec4 color=glm::vec4(1)) const {
        renderer.draw_image(t,(ox+x*scale)/renderer.frame_size.width,(oy+y*scale)/renderer.frame_size.height,
            w*scale/renderer.frame_size.height,h*scale/renderer.frame_size.height,color);
    }
    void glass_image(const auroreengine::texture& t,float x,float y,float w,float h,glm::vec4 color) const {
        renderer.draw_image_glass(t,(ox+x*scale)/renderer.frame_size.width,(oy+y*scale)/renderer.frame_size.height,
            w*scale/renderer.frame_size.height,h*scale/renderer.frame_size.height,color);
    }
    static float pulse() {
        const auto t=std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
        return 0.7f+0.55f*(0.5f+0.5f*std::sin(t*1.848f));
    }
};
class media_icons {
    struct item { std::unique_ptr<auroreengine::texture> image,normal;std::future<void> ready,normal_ready; };
    std::map<std::string,item,std::less<>> images;
public:
    ~media_icons() { for(auto& [key,value]:images) {if(value.ready.valid()) value.ready.wait();if(value.normal_ready.valid())value.normal_ready.wait();} }
    void load(auroreengine::resource_loader& loader,std::string name) {
        if(images.contains(name)) return;
        auto& value=images[name];
        value.image=std::make_unique<auroreengine::texture>(loader.getDevice(),loader.getAllocator());
        value.ready=loader.loadTexture(value.image.get(),config::CONFIG.asset_directory/"icons"/(name+".png"));
        const auto normals=config::CONFIG.asset_directory/"normalmaps"/(name+".png");
        if(name.starts_with("theme/")&&std::filesystem::is_regular_file(normals)) {
            value.normal=std::make_unique<auroreengine::texture>(loader.getDevice(),loader.getAllocator(),vk::ImageUsageFlagBits::eSampled,vk::Format::eR8G8B8A8Unorm);
            value.normal_ready=loader.loadTexture(value.normal.get(),normals);
        }
    }
    void box(const xmb_canvas& c,std::string_view name,float x,float y,float width,float height,float alpha=1) const {
        const auto it=images.find(name);if(it!=images.end()&&it->second.image->loaded) c.image(*it->second.image,x,y,width,height,{1,1,1,alpha});
    }
    float scaled_width(std::string_view name,float height) const {
        const auto it=images.find(name);
        if(it==images.end()||!it->second.image->loaded) return height;
        const auto& image=*it->second.image;
        return height*image.width/std::max(1,image.height);
    }
    void draw(const xmb_canvas& c,std::string_view name,float cx,float cy,float height,float alpha=0.85f,float focus=1) const {
        const auto it=images.find(name);if(it==images.end()||!it->second.image->loaded) return;
        const auto& t=*it->second.image;
        const float h=height*focus,w=h*t.width/std::max(1,t.height);
        if(config::CONFIG.iconGlassRefraction&&it->second.normal&&it->second.normal->loaded)
            c.glass_image(*it->second.normal,cx-w*0.5f,cy-h*0.5f,w,h,{utils::xmb_icon_colour(openxmb::xmb::wall_clock_now()),alpha});
        else c.image(t,cx-w*0.5f,cy-h*0.5f,w,h,{1,1,1,alpha});
    }
};
}
