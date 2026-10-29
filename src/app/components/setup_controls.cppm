module;
#include "../../localization/shell_locale.hpp"
#include "openxmb/setup/value_motion.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

export module openxmb.app:setup_controls;
import :xmb_presentation;
import :dialog_chrome;
import auroreengine;
import glm;
import openxmb.config;

export namespace app::setup_controls {

inline float srgb_linear(float value) {
    const float v=std::clamp(value,0.0F,1.0F);
    return v<=0.04045F?v/12.92F:std::pow((v+0.055F)/1.055F,2.4F);
}
inline float width(const xmb_canvas& c,std::string_view value,float size,bool localize=true) {
    const auto shown=localize?openxmb::localization::shell_locale::translate(value):std::string(value);
    return c.renderer.text_width_pixels(shown,size*c.scale*config::CONFIG.textScale)/c.scale;
}
inline void text(const xmb_canvas& c,std::string_view value,float x,float baseline,float size,
                 bool focused=false,float alpha=1,bool centered=false,bool localize=true) {
    const auto shown=localize?openxmb::localization::shell_locale::translate(value):std::string(value);
    c.text(std::string_view(shown),x,baseline,size,{1,1,1,alpha},focused?0.54F*xmb_canvas::pulse():0,centered);
}
// Approximate cap height of the shell's sans-serif at a design size.
inline float cap_height(float size) {
    return 0.72F*size*static_cast<float>(config::CONFIG.textScale);
}
// Text-local selection light from the 1080p setup stills (list row, date
// segment, Yes/No): a soft white wash over the focused text's cap band that
// reads ~0.4 sRGB between the glyphs together with the glyph halo and decays
// by e every ~2.6 px outside it. It breathes with the glyph halo.
inline void focus_glow(const xmb_canvas& c,float left,float baseline,float w,float size,float alpha=1) {
    const float cap=cap_height(size);
    c.glow(left+1,baseline-cap,std::max(w-2,0.0F),cap,
           {1,1,1,srgb_linear(0.36F)*xmb_canvas::pulse()*alpha},3.0F,2.6F,1.0F);
}
inline void selectable_text(const xmb_canvas& c,std::string_view value,float x,float baseline,float size,
                            bool focused,float alpha=1,bool centered=false,bool localize=true) {
    if(focused) {
        const float w=width(c,value,size,localize);
        focus_glow(c,centered?x-w*0.5F:x,baseline,w,size,alpha);
    }
    text(c,value,x,baseline,size,focused,alpha,centered,localize);
}
inline void changing_text(const xmb_canvas& c,std::string_view previous,std::string_view current,
                          float x,float baseline,float size,const openxmb::setup::ValueMotion& motion,
                          bool focused,float alpha=1) {
    if(previous==current) {text(c,current,x,baseline,size,focused,alpha,true,false);return;}
    if(motion.outgoing_alpha>0)
        text(c,previous,x,baseline+motion.outgoing_offset,size,focused,alpha*motion.outgoing_alpha,true,false);
    if(motion.incoming_alpha>0)
        text(c,current,x,baseline+motion.incoming_offset,size,focused,alpha*motion.incoming_alpha,true,false);
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
// Date-segment spinner mark: ~13 px wide and 7 px tall in the reference,
// pointing away from the digits from a base at base_y, with a faint rim
// standing in for the recording's soft edge.
inline void spin_arrow(const xmb_canvas& c,float x,float base_y,bool up,float alpha=1) {
    const float s=up?-1.0F:1.0F;
    triangle(c,{x,base_y+s*8.0F},{x-7.7F,base_y-s*0.6F},{x+7.7F,base_y-s*0.6F},{1,1,1,srgb_linear(0.40F)*alpha});
    triangle(c,{x,base_y+s*7.0F},{x-6.5F,base_y},{x+6.5F,base_y},{1,1,1,alpha});
}
// Page arrows after a 25 fps trace of the reference recording. Both arrows
// repeat a 2.52 s outward sweep in mirror image: the tip appears 252 px from
// its screen edge and drifts 76 px outwards over 1.76 s, brightening with an
// ease-out over 0.84 s, holding until 1.36 s and fading out linearly by
// 2.28 s; it then stays hidden until the cycle restarts. The shape is ~28 px
// long and ~26 px tall at its base, peaking at ~0.65 sRGB at the tip and
// fading evenly (in sRGB) to nothing at the base.
inline void page_arrow(const xmb_canvas& c,bool left,float alpha=1) {
    constexpr int period_ms=2520;
    constexpr float travel=1.76F,rise=0.84F,hold=1.36F,gone=2.28F;
    constexpr float start=252,drift=76,length=28,half_height=13,peak=0.65F;
    constexpr int slices=4;
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()%period_ms;
    const float t=static_cast<float>(ms)/1000.0F;
    const float level=t<rise?1-(1-t/rise)*(1-t/rise):t<hold?1.0F:t<gone?(gone-t)/(gone-hold):0.0F;
    if(level<=0||alpha<=0) return;
    const float edge=start-drift*std::min(t/travel,1.0F);
    const float tip=left?edge:1920.0F-edge,toward=left?1.0F:-1.0F;
    using V=auroreengine::simple_renderer::vertex_data;
    const auto point=[&](float x,float y) {return glm::vec2{(c.ox+x*c.scale)/c.renderer.frame_size.width,
                                                        (c.oy+y*c.scale)/c.renderer.frame_size.height};};
    const auto colour=[&](float u) {return glm::vec4{1,1,1,srgb_linear(peak*level*(1-u))*alpha};};
    const auto vertex=[&](float u,float side) {
        return V{point(tip+toward*length*u,540+side*half_height*u),colour(u),{u,side*0.5F+0.5F}};
    };
    std::array<V,3+6*(slices-1)> vertices;
    std::size_t n=0;
    vertices[n++]=vertex(0,0);vertices[n++]=vertex(1.0F/slices,-1);vertices[n++]=vertex(1.0F/slices,1);
    for(int i=1;i<slices;++i) {
        const float u0=static_cast<float>(i)/slices,u1=static_cast<float>(i+1)/slices;
        const V a=vertex(u0,-1),b=vertex(u0,1),d=vertex(u1,-1),e=vertex(u1,1);
        vertices[n++]=a;vertices[n++]=d;vertices[n++]=b;
        vertices[n++]=b;vertices[n++]=d;vertices[n++]=e;
    }
    c.renderer.draw_generic(std::span{vertices});
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
    selectable_text(c,label,x+33,y+18,22,focused,alpha);
}
// Setup name field after the 1080p reference still: a crisp ~1.5 px rim at
// ~0.21 sRGB around a flat ~0.05 sRGB interior, corners rounded ~4 px and no
// inner glow. Text placement matches the shared message field. The setup
// stage is black, so sRGB levels convert directly to linear alpha.
inline void field(const xmb_canvas& c,float x,float y,float w,float h,std::string_view value,float alpha=1) {
    const float rim=srgb_linear(0.21F),fill=srgb_linear(0.05F);
    c.shape(x,y,w,h,{1,1,1,rim*alpha},4.0F,0.8F,fill/rim);
    auto& r=c.renderer;
    const float pixels=dialog_chrome::field_text_size*c.scale*static_cast<float>(config::CONFIG.textScale);
    const auto metrics=r.text_line_metrics_pixels(pixels)/c.scale;
    const float baseline=y+h*0.5F+(metrics.x-metrics.y)*0.5F;
    r.set_clip((c.ox+(x+2)*c.scale)/r.frame_size.width,(c.oy+y*c.scale)/r.frame_size.height,
               (w-4)*c.scale/r.frame_size.width,h*c.scale/r.frame_size.height);
    c.text(value,x+7,baseline,dialog_chrome::field_text_size,{1,1,1,alpha});
    r.reset_clip();
}
}
