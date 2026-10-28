module;
#include "../../localization/shell_locale.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>

export module openxmb.app:setup_controls;
import :xmb_presentation;
import :dialog_chrome;
import auroreengine;
import glm;
import openxmb.config;

export namespace app::setup_controls {

inline float width(const xmb_canvas& c,std::string_view value,float size,bool localize=true) {
    const auto shown=localize?openxmb::localization::shell_locale::translate(value):std::string(value);
    return c.renderer.text_width_pixels(shown,size*c.scale*config::CONFIG.textScale)/c.scale;
}
inline void text(const xmb_canvas& c,std::string_view value,float x,float baseline,float size,
                 bool focused=false,float alpha=1,bool centered=false,bool localize=true) {
    const auto shown=localize?openxmb::localization::shell_locale::translate(value):std::string(value);
    c.text(std::string_view(shown),x,baseline,size,{1,1,1,alpha},focused?0.48F*xmb_canvas::pulse():0,centered);
}
inline void triangle(const xmb_canvas& c,glm::vec2 a,glm::vec2 b,glm::vec2 d,glm::vec4 colour) {
    using V=auroreengine::simple_renderer::vertex_data;
    const auto point=[&](glm::vec2 p) {return glm::vec2{(c.ox+p.x*c.scale)/c.renderer.frame_size.width,
                                                      (c.oy+p.y*c.scale)/c.renderer.frame_size.height};};
    c.renderer.draw_generic(std::array{V{point(a),colour,{0,0}},V{point(b),colour,{1,0}},V{point(d),colour,{0,1}}});
}
inline void vertical_arrow(const xmb_canvas& c,float x,float y,bool up,float alpha=1) {
    triangle(c,{x,y+(up?-6.0F:6.0F)},{x-6,y+(up?5.0F:-5.0F)},
             {x+6,y+(up?5.0F:-5.0F)},{1,1,1,alpha});
}
inline void page_arrow(const xmb_canvas& c,bool left,float alpha=1) {
    const float tip=left?200.0F:1720.0F,base=left?235.0F:1685.0F;
    using V=auroreengine::simple_renderer::vertex_data;
    const auto point=[&](float x,float y) {return glm::vec2{(c.ox+x*c.scale)/c.renderer.frame_size.width,
                                                        (c.oy+y*c.scale)/c.renderer.frame_size.height};};
    const glm::vec4 bright{1,1,1,0.34F*alpha},clear{1,1,1,0};
    c.renderer.draw_generic(std::array{V{point(tip,540),bright,{0,0}},V{point(base,529),clear,{1,0}},V{point(base,551),clear,{0,1}}});
}
inline void scrollbar(const xmb_canvas& c,float x,float top,float height,std::size_t count,
                      std::size_t visible,std::size_t first,float alpha=1) {
    if(count==0) return;
    c.rect(x-5,top,10,height,{0.035F,0.035F,0.035F,alpha});
    c.rect(x-2,top,2,height,{0.07F,0.07F,0.07F,alpha});
    const float thumb=count<=visible?height:std::min(height,7.0F);
    const auto max_first=count>visible?count-visible:0;
    const float p=max_first?static_cast<float>(std::min(first,max_first))/static_cast<float>(max_first):0;
    c.rect(x-5,top+(height-thumb)*p,10,thumb,{0.9F,0.9F,0.9F,alpha});
    if(first>0) vertical_arrow(c,x,top-15,true,alpha);
    if(first+visible<count) vertical_arrow(c,x,top+height+15,false,alpha);
}
inline void stroke(const xmb_canvas& c,glm::vec2 a,glm::vec2 b,float thickness,float alpha) {
    const auto delta=b-a;
    const float length=std::hypot(delta.x,delta.y);
    if(length<=0) return;
    const glm::vec2 normal{-delta.y*thickness/(2*length),delta.x*thickness/(2*length)};
    triangle(c,a+normal,a-normal,b+normal,{1,1,1,alpha});
    triangle(c,b+normal,a-normal,b-normal,{1,1,1,alpha});
}
inline void checkbox(const xmb_canvas& c,float x,float y,std::string_view label,bool checked,
                     bool focused,float alpha=1) {
    if(focused) c.glow(x,y,21,21,{1,1,1,0.35F*alpha},2,4,0);
    c.rect(x,y,21,21,{0.8F,0.8F,0.8F,alpha},2);
    c.rect(x+2,y+2,17,17,{0.035F,0.035F,0.035F,alpha},1);
    if(checked) {
        stroke(c,{x+5,y+10},{x+9,y+15},2.2F,alpha);
        stroke(c,{x+9,y+15},{x+17,y+6},2.2F,alpha);
    }
    text(c,label,x+33,y+18,22,focused,alpha);
}
inline void field(const xmb_canvas& c,float x,float y,float w,float h,std::string_view value,float alpha=1) {
    dialog_chrome::text_field(c,c.renderer,x,y,w,h,value,{.focus=0,.pressed=0,.alpha=alpha});
}
}
