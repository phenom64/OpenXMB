module;
#include "../../localization/shell_locale.hpp"

#include "dialog_input.hpp"
#include "dialog_text_layout.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

export module openxmb.app:text_entry;
import :component;
import :main;
import :dialog_chrome;
import :xmb_presentation;
import openxmb.utils;
import openxmb.config;
import auroreengine;
import glm;

export namespace app {

// Where the keyboard was opened from. Over a message screen the PS3 darkens the
// whole scene and centres the keyboard; over the crossbar (Internet Search) the
// menu recedes and fades, the wave stays bright and the keyboard sits lower
// right. Positions are 1920x1080 estimates from the RPCS3 captures.
enum class keyboard_context { message, xmb };

class text_entry_overlay : public component, public event_receiver {
public:
    text_entry_overlay(std::string title, std::string initial,
                       std::function<void(std::string)> accept,std::function<void()> cancel={},
                       keyboard_context context=keyboard_context::xmb)
        : title_(::openxmb::localization::shell_locale::translate(title)), editor_(std::move(initial)), accept_(std::move(accept)),cancel_(std::move(cancel)),
          context_(context), position_(context==keyboard_context::message?glm::vec2{493,290}:glm::vec2{873,488}) {
        // Every captured keyboard opened with "g" focused.
        editor_.row=2;editor_.col=4;
    }

    result on_event(const event& e) override {
        if(closing_) return result::success;
        if(const auto* stick=e.get<events::joystick_axis>()) {
            if(stick->index==events::logical_joystick_index::right) {stick_={stick->x,stick->y};return result::success;}
            return result::unsupported;
        }
        if(const auto* button=e.get<events::controller_button_down>()) {
            using button_t=events::logical_controller_button;
            switch(button->button) {
            case button_t::start:return finish(true);
            case button_t::back:editor_.cycle_page();return result::success|result::ok_sound;
            case button_t::leftshoulder:editor_.move_caret(-1);return result::success;
            case button_t::rightshoulder:editor_.move_caret(1);return result::success;
            default:break;
            }
            // The controller Triangle inserts a space on the PS3 keyboard;
            // keyboard-bound Options keeps its existing Save meaning.
            if(e.action==action::options) {editor_.insert(" ");return result::success;}
        }
        std::optional<unsigned> down,up;
        if(const auto* key=e.get<events::key_down>())down=key->keycode;
        if(const auto* key=e.get<events::key_up>())up=key->keycode;
        // Semantic bindings win over printable keys and transport shortcuts.
        const auto command=(down||e.is<events::controller_button_down>())?dialog_input::mapped(e.action):dialog_input::Command::none;
        const bool moved=command==dialog_input::Command::up||command==dialog_input::Command::down||
                         command==dialog_input::Command::left||command==dialog_input::Command::right;
        switch(editor_.event(command,down,up)) {
        case dialog_input::Outcome::accept:return finish(true);
        case dialog_input::Outcome::cancel:return finish(false);
        case dialog_input::Outcome::copy:copy_value_=true;return result::success|result::ok_sound;
        case dialog_input::Outcome::paste:paste_value_=true;return result::success|result::ok_sound;
        default:return moved?result::success|result::ok_sound:result::success;
        }
    }

    result tick(shell* xmb) override {
        const auto now=std::chrono::steady_clock::now();
        const float dt=std::clamp(std::chrono::duration<float>(now-last_tick_).count(),0.0F,0.1F);
        last_tick_=now;
        // Right-stick repositioning is supported by the reference; speed and
        // interpolation were not measured, so one moderate rate is used and
        // the only limit is keeping the whole keyboard on screen.
        glm::vec2 deflection=stick_;
        const float magnitude=std::hypot(deflection.x,deflection.y);
        deflection=magnitude<stick_deadzone?glm::vec2{}:deflection/magnitude*std::pow((std::min(magnitude,1.0F)-stick_deadzone)/(1.0F-stick_deadzone),1.6F);
        position_+=deflection*stick_speed*dt;
        position_=glm::clamp(position_,glm::vec2{0,0},glm::vec2{1920-panel_width,1080-panel_height});
        if(copy_value_) {copy_value_=false;xmb->set_clipboard(std::string(editor_.value));}
        if(paste_value_) {
            paste_value_=false;
            if(const auto& clip=xmb->get_clipboard()) {
                if(const auto* text=std::get_if<std::string>(&*clip)) editor_.insert(*text);
                else if(const auto* source=std::get_if<std::function<std::string()>>(&*clip)) editor_.insert((*source)());
            }
        }
        if(closing_&&dialog_chrome::seconds_since(closed_at_)>=exit_seconds) {
            if(accepted_) {accept_(editor_.value);return result::close|result::confirm_sound;}
            if(cancel_)cancel_();
            return result::close|result::back_sound;
        }
        return result::success;
    }

    void render(auroreengine::gui_renderer& r, shell* xmb) override {
        xmb_canvas c(r);
        const float age=dialog_chrome::seconds_since(opened_at_);
        const float exit=closing_?dialog_chrome::smooth01(dialog_chrome::seconds_since(closed_at_)/exit_seconds):0.0F;
        // Entrance (one trial each from the playlist and Internet Search
        // captures): the panel appears slightly enlarged and settles within a
        // few frames, the key labels follow, and the backdrop keeps darkening
        // for roughly 0.4 s. The exit is not captured; it simply reverses.
        const float panel_alpha=dialog_chrome::smooth01(age/0.08F)*(1.0F-exit);
        const float labels=dialog_chrome::smooth01((age-0.035F)/0.07F)*(1.0F-exit);
        const float settle=1.0F-std::pow(1.0F-std::clamp(age/0.11F,0.0F,1.0F),3.0F);
        const float zoom=1.0F+0.06F*(1.0F-settle)+0.03F*exit;
        if(context_==keyboard_context::message)
            c.rect(0,0,1920,1080,{0,0,0,dialog_chrome::shade_alpha(0.71F*(1.0F-std::pow(1.0F-std::clamp(age/0.4F,0.0F,1.0F),2.0F))*(1.0F-exit))});
        if(panel_alpha<=0.001F) return;

        const glm::vec2 centre=position_+glm::vec2{panel_width,panel_height}*0.5F;
        const auto map=[&](float x,float y){return centre+(position_+glm::vec2{x,y}-centre)*zoom;};
        const auto rect=[&](float x,float y,float w,float h,glm::vec4 colour,float radius=0) {
            const auto p=map(x,y);colour=dialog_chrome::linear(colour);colour.a*=panel_alpha;c.rect(p.x,p.y,w*zoom,h*zoom,colour,radius*zoom);
        };
        const auto gradient=[&](float x,float y,float w,float h,glm::vec4 top,glm::vec4 bottom) {
            const auto p=map(x,y);top=dialog_chrome::linear(top);bottom=dialog_chrome::linear(bottom);top.a*=panel_alpha;bottom.a*=panel_alpha;c.gradient(p.x,p.y,w*zoom,h*zoom,top,bottom);
        };
        const auto text=[&](std::string_view value,float x,float baseline,float size,glm::vec4 colour,bool centred=false) {
            const auto p=map(x,baseline);colour=dialog_chrome::linear(colour);colour.a*=panel_alpha;c.text(value,p.x,p.y,size*zoom,colour,0,centred);
        };
        const auto text_right=[&](std::string_view value,float right,float baseline,float size,glm::vec4 colour) {
            const float width=r.text_width_pixels(value,size*c.scale*config::CONFIG.textScale)/c.scale;
            text(value,right-width,baseline,size,colour);
        };
        const auto triangle=[&](glm::vec2 a,glm::vec2 b,glm::vec2 d,glm::vec4 colour) {
            colour=dialog_chrome::linear(colour);colour.a*=panel_alpha;
            const auto n=[&](glm::vec2 v){const auto p=map(v.x,v.y);return glm::vec2{(c.ox+p.x*c.scale)/r.frame_size.width,(c.oy+p.y*c.scale)/r.frame_size.height};};
            r.draw_generic(std::array{auroreengine::simple_renderer::vertex_data{n(a),colour,{0,0}},
                auroreengine::simple_renderer::vertex_data{n(b),colour,{1,0}},auroreengine::simple_renderer::vertex_data{n(d),colour,{0,1}}});
        };
        const auto image=[&](const auroreengine::texture& t,float x,float y,float w,float h,float alpha) {
            const auto p=map(x,y);c.image(t,p.x,p.y,w*zoom,h*zoom,{1,1,1,alpha*panel_alpha});
        };
        const auto grey=[](float v,float a=1.0F){return glm::vec4(v,v,v,a);};
        const glm::vec4 ink{0.09F,0.09F,0.09F,1},navy{0.08F,0.20F,0.36F,1};

        // Colours below are sRGB values sampled from the captures (see
        // dialog_chrome::linear). Frame, title bar (slightly translucent: the message prompt showed
        // through it in the capture) and the dark entry strip.
        rect(-1,-1,panel_width+2,panel_height+2,{0,0,0,0.35F},3);
        gradient(0,0,panel_width,46,grey(0.59F,0.8F),grey(0.53F,0.8F));
        rect(0,0,panel_width,1,grey(0.72F,0.8F));
        draw_pencil(triangle,{11,9});
        text(dialog_text::ellipsize(title_,840*c.scale,[&](auto v){return r.text_width_pixels(v,26*c.scale*config::CONFIG.textScale);}),39,31,26,{1,1,1,1});
        gradient(0,46,panel_width,57,grey(0.34F),grey(0.18F));
        const float input_pixels=22*c.scale*config::CONFIG.textScale;
        const std::string before=editor_.value.substr(0,editor_.caret()),after=editor_.value.substr(editor_.caret());
        const auto shown=dialog_text::tail(before,820*c.scale,[&](auto v){return r.text_width_pixels(v,input_pixels);});
        text(shown,19,81,22,{1,1,1,1});
        const float caret_x=19+r.text_width_pixels(shown,input_pixels)/c.scale+2;
        draw_caret(triangle,caret_x,74);
        if(!after.empty()) text(dialog_text::ellipsize(after,std::max(0.0F,(panel_width-caret_x-40))*c.scale,[&](auto v){return r.text_width_pixels(v,input_pixels);}),caret_x+16,81,22,{1,1,1,1});
        rect(0,103,panel_width,8,grey(0.59F));

        if(!config::CONFIG.enabled("input.osk")) {
            gradient(0,111,panel_width,44,grey(0.32F),grey(0.27F));
            rect(0,155,panel_width,10,grey(0.39F));
            return;
        }

        rect(0,111,panel_width,333,grey(0.58F));
        for(int row=0;row<dialog_input::TextEntry::character_rows;++row) for(int col=0;col<dialog_input::TextEntry::columns;++col) {
            const float x=key_x(col),y=key_y(row);
            const bool focused=row==editor_.row&&col==editor_.col;
            draw_key(gradient,rect,x,y,key_width,row_height(row),focused?glm::vec3(0.94F):glm::vec3(0.77F),focused);
            if(labels>0) text_right(editor_.character(row,col),x+55,y+41,25,{ink.r,ink.g,ink.b,labels});
        }
        // Prediction pane (no candidates are generated) and its footer strip.
        rect(725,113,202,232,grey(0.44F));
        rect(917,116,7,226,grey(0.52F));
        gradient(725,348,202,30,grey(0.24F),grey(0.18F));
        if(labels>0) {
            text("Prediction",735,369,17,{0.84F,0.9F,0.95F,labels});
            rect(870,352,24,22,grey(0.62F,labels),2);rect(898,352,24,22,grey(0.85F,labels),2);
            text("a",882,369,16,{1,1,1,labels},true);text("0",910,369,16,{0.1F,0.1F,0.1F,labels},true);
        }

        const glm::vec3 yellow{0.70F,0.67F,0.55F},blue{0.54F,0.67F,0.70F},green{0.54F,0.69F,0.62F};
        const auto function_key=[&](int row,const dialog_input::TextEntry::Cell& item) {
            using Key=dialog_input::TextEntry::Key;
            const float x=key_x(item.first),y=key_y(row),w=key_x(item.last)+key_width-x,h=row_height(row);
            const bool focused=editor_.row==row&&item.first<=editor_.col&&editor_.col<=item.last;
            glm::vec3 tone=row==4?(item.first<4?yellow:blue):(item.key==Key::keypad||item.key==Key::blank||item.key==Key::input_mode||item.key==Key::enter?green:blue);
            if(item.key==Key::keypad) tone*=0.82F;
            if(row==5&&item.first>=10) {const float px=item.first==10?723.0F:829.0F;draw_key(gradient,rect,px,y,98,h,focused?glm::vec3(1.0F):tone,focused);}
            else draw_key(gradient,rect,x,y,w,h,focused?glm::mix(tone,glm::vec3(1.0F),0.65F):tone,focused);
            if(labels<=0) return;
            const glm::vec4 label{navy.r,navy.g,navy.b,labels};
            const float right=x+w-9,base=y+h-9;
            switch(item.key) {
            case Key::symbols:text("@#:",x+6,base,21,label);break;
            case Key::accents:badge(rect,text,x+3,y+3,"L2",labels);text_right("a/\xC3\xA0",right,base,20,label);break;
            case Key::caps:badge(rect,text,x+3,y+3,"R2",labels);text_right("a/A",right,base,20,label);break;
            case Key::backspace:icon_badge(xmb,rect,image,triangle,x+3,y+3,action::extra,labels);text_right("Backspace",right,base,20,label);break;
            case Key::space:icon_badge(xmb,rect,image,triangle,x+3,y+3,action::options,labels);text_right("Space",right,base,20,label);break;
            case Key::up:arrow(triangle,x+w*0.5F,y+h*0.62F,0,label);break;
            case Key::down:arrow(triangle,x+w*0.5F,y+h*0.62F,1,label);break;
            case Key::caret_left:badge(rect,text,x+3,y+3,"L1",labels);arrow(triangle,x+w*0.68F,y+h*0.62F,2,label);break;
            case Key::caret_right:badge(rect,text,x+3,y+3,"R1",labels);arrow(triangle,x+w*0.68F,y+h*0.62F,3,label);break;
            case Key::keypad:for(int i=0;i<9;++i)rect(x+19+(i%3)*9,y+14+(i/3)*7,7,5,label);break;
            case Key::input_mode:badge(rect,text,x+3,y+3,"SELECT",labels);text_right(page_name(),right,base,20,label);break;
            case Key::enter:badge(rect,text,x+3,y+3,"START",labels);text_right("Enter",right,base,20,label);break;
            case Key::copy:{const float px=723;rect(px+40,y+12,18,16,label);rect(px+46,y+17,18,16,{label.r,label.g,label.b,label.a*0.6F});break;}
            case Key::paste:{const float px=829;const glm::vec4 dim{label.r,label.g,label.b,label.a*(xmb->get_clipboard()?1.0F:0.35F)};rect(px+42,y+12,16,21,dim,2);rect(px+46,y+9,8,5,dim);break;}
            default:break;
            }
        };
        for(const auto& item:dialog_input::TextEntry::function_row) function_key(4,item);
        for(const auto& item:dialog_input::TextEntry::control_row) function_key(5,item);

        gradient(0,444,panel_width,44,grey(0.32F),grey(0.27F));
        gradient(0,488,panel_width,12,grey(0.38F),grey(0.41F));
    }
    bool is_opaque() const override { return false; }
    // Over the crossbar the menu recedes behind the keyboard (Internet Search).
    [[nodiscard]] float menu_recede() const override {
        if(context_!=keyboard_context::xmb) return 0.0F;
        const float in=dialog_chrome::smooth01(dialog_chrome::seconds_since(opened_at_)/0.12F);
        const float out=closing_?dialog_chrome::smooth01(dialog_chrome::seconds_since(closed_at_)/exit_seconds):0.0F;
        return in*(1.0F-out);
    }
private:
    static constexpr float panel_width=934,panel_height=500,key_width=64;
    static constexpr float exit_seconds=0.14F,stick_deadzone=0.25F,stick_speed=900.0F;
    static float key_x(int col) {return 4+72*static_cast<float>(col);}
    static float key_y(int row) {constexpr std::array<float,6> rows{113,168,222,277,332,386};return rows[static_cast<std::size_t>(row)];}
    static float row_height(int row) {return row<4?50.0F:46.0F;}
    std::string_view page_name() const {return editor_.page==0?"Input Mode":editor_.page==1?"Symbols":"Accents";}

    result finish(bool accepted) {
        closing_=true;accepted_=accepted;closed_at_=std::chrono::steady_clock::now();
        return result::success;
    }
    static void draw_key(const auto& gradient,const auto& rect,float x,float y,float w,float h,glm::vec3 tone,bool focused) {
        const auto shade=[&](float k){return glm::vec4(glm::min(tone*k,glm::vec3(1.0F)),1.0F);};
        if(focused) rect(x-3,y-3,w+6,h+6,{1,1,1,0.28F},5);
        rect(x,y,w,h,shade(0.74F),3);
        gradient(x+1,y+1,w-2,h*0.34F,shade(0.80F),shade(1.0F));
        gradient(x+1,y+1+h*0.34F,w-2,h*0.36F,shade(1.0F),shade(1.0F));
        gradient(x+1,y+h*0.70F,w-2,h*0.30F-1,shade(0.98F),shade(0.86F));
    }
    static void badge(const auto& rect,const auto& text,float x,float y,std::string_view label,float alpha) {
        const float w=label.size()>2?60.0F:23.0F;
        rect(x,y,w,15,{0.16F,0.16F,0.16F,0.92F*alpha},7);
        text(label,x+w*0.5F,y+12,12,{1,1,1,alpha},true);
    }
    // Controller glyph badge: the shell's own button art when loaded (so the
    // hint follows the connected controller), else a drawn square/triangle.
    static void icon_badge(shell* xmb,const auto& rect,const auto& image,const auto& triangle,float x,float y,action a,float alpha) {
        rect(x,y,21,18,{0.16F,0.16F,0.16F,0.92F*alpha},6);
        if(const auto* icon=xmb->get_button_icon(a);icon&&icon->loaded) {image(*icon,x+3,y+2,15,15,alpha);return;}
        const glm::vec4 white{1,1,1,alpha},dark{0.16F,0.16F,0.16F,alpha};
        if(a==action::options) {triangle({x+10.5F,y+3},{x+4,y+15},{x+17,y+15},white);triangle({x+10.5F,y+6},{x+6.5F,y+13.5F},{x+14.5F,y+13.5F},dark);return;}
        rect(x+5,y+4,11,11,white);rect(x+6.5F,y+5.5F,8,8,dark);
    }
    static void arrow(const auto& triangle,float cx,float cy,int direction,glm::vec4 colour) {
        constexpr float s=7.0F;
        switch(direction) {
        case 0:triangle({cx-s,cy+s*0.6F},{cx+s,cy+s*0.6F},{cx,cy-s*0.6F},colour);break;
        case 1:triangle({cx-s,cy-s*0.6F},{cx+s,cy-s*0.6F},{cx,cy+s*0.6F},colour);break;
        case 2:triangle({cx+s*0.6F,cy-s},{cx+s*0.6F,cy+s},{cx-s*0.6F,cy},colour);break;
        default:triangle({cx-s*0.6F,cy-s},{cx-s*0.6F,cy+s},{cx+s*0.6F,cy},colour);break;
        }
    }
    // Outline "◁" insertion mark drawn after the text, as on the PS3.
    static void draw_caret(const auto& triangle,float x,float cy) {
        const glm::vec4 white{1,1,1,1},fill{0.22F,0.22F,0.22F,1};
        triangle({x+13,cy-9},{x+13,cy+9},{x,cy},white);
        triangle({x+11.5F,cy-6},{x+11.5F,cy+6},{x+3.5F,cy},fill);
    }
    static void draw_pencil(const auto& triangle,glm::vec2 at) {
        const glm::vec4 white{1,1,1,1},edge{0.15F,0.15F,0.15F,0.8F};
        const glm::vec2 a=at+glm::vec2{14,0},b=at+glm::vec2{20,6},d=at+glm::vec2{5,21},e=at+glm::vec2{-1,15};
        triangle(a+glm::vec2{-1,-1},b+glm::vec2{1,1},d+glm::vec2{1,1},edge);triangle(a+glm::vec2{-1,-1},d+glm::vec2{1,1},e+glm::vec2{-1,-1},edge);
        triangle(a,b,d,white);triangle(a,d,e,white);
        triangle(e,d,at+glm::vec2{-3,24},white);
    }

    std::string title_;
    dialog_input::TextEntry editor_;
    std::function<void(std::string)> accept_;
    std::function<void()> cancel_;
    keyboard_context context_;
    glm::vec2 position_;
    glm::vec2 stick_{};
    bool closing_{},accepted_{},copy_value_{},paste_value_{};
    std::chrono::steady_clock::time_point opened_at_{std::chrono::steady_clock::now()},closed_at_{},last_tick_{std::chrono::steady_clock::now()};
};
}
