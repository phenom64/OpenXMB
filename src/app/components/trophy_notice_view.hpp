#pragma once
#include "dialog_text_layout.hpp"
#include "openxmb/xmb/trophy_notice.hpp"
#include <algorithm>
#include <cmath>
#include <string_view>

namespace app::trophy_notice_view {
// Resolved content of the front notice. Strings are already translated. The
// art column is reserved whenever art_slot is set; art is drawn only once the
// caller has a texture whose upload has completed (null otherwise).
template<class Texture,class Colour> struct Notice {
    std::string_view label,grade;
    Colour grade_colour;
    std::string_view title;
    bool art_slot{};
    const Texture* art{};
};
// The single banner renderer for the in-shell notice and the passive host.
// Coordinates are framebuffer pixels of the renderer's current target extent;
// scale maps 1080p typography. Renderer is the engine's gui_renderer.
template<class Renderer,class Texture,class Colour>
void draw(Renderer& renderer,const Notice<Texture,Colour>& notice,float alpha,float x,float y,float w,float h,float scale) {
    if(alpha<=0.0F)return;
    const auto frame_w=static_cast<float>(renderer.frame_size.width),frame_h=static_cast<float>(renderer.frame_size.height);
    renderer.draw_rect({x/frame_w,y/frame_h},{w/frame_w,h/frame_h},{0,0,0,0.8F*alpha});
    const float pad=h*0.12F,icon=h-2.0F*pad;
    if(notice.art)
        renderer.draw_image_sized(*notice.art,(x+pad)/frame_w,(y+pad)/frame_h,static_cast<int>(std::lround(icon)),static_cast<int>(std::lround(icon)),{1,1,1,alpha});
    const float text_x=notice.art_slot?x+2.0F*pad+icon:x+w*(0.015F/openxmb::xmb::trophy_notice_frame.width);
    const float line_size=24*scale,title_size=26*scale,label_y=y+h*(0.03F/0.09F),title_y=y+h*(0.07F/0.09F);
    renderer.draw_text_pixels(notice.label,text_x,label_y,line_size,{1,1,1,alpha},0.7F);
    if(!notice.grade.empty())
        renderer.draw_text_pixels(notice.grade,text_x+renderer.text_width_pixels(notice.label,line_size)+12*scale,label_y,line_size,notice.grade_colour,0.7F);
    const auto title=dialog_text::ellipsize(notice.title,std::max(0.0F,x+w-pad-text_x),[&](auto value){return renderer.text_width_pixels(value,title_size);});
    renderer.draw_text_pixels(title,text_x,title_y,title_size,{1,1,1,alpha},0.7F);
}
} // namespace app::trophy_notice_view
