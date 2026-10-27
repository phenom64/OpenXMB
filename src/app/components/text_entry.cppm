module;

#include "dialog_input.hpp"
#include "dialog_text_layout.hpp"
#include <optional>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

export module openxmb.app:text_entry;
import :component;
import :main;
import :xmb_presentation;
import openxmb.utils;
import openxmb.config;
import auroreengine;
import glm;

export namespace app {

class text_entry_overlay : public component, public event_receiver {
public:
    float typography_scale() const override { return 2.5F; }

    text_entry_overlay(std::string title, std::string initial,
                       std::function<void(std::string)> accept,std::function<void()> cancel={})
        : title_(std::move(title)), editor_(std::move(initial)), accept_(std::move(accept)),cancel_(std::move(cancel)) {}

    result on_event(const event& e) override {
        std::optional<unsigned> down,up;
        if(const auto* key=e.get<events::key_down>())down=key->keycode;
        if(const auto* key=e.get<events::key_up>())up=key->keycode;
        // Semantic bindings win over printable keys and transport shortcuts.
        const auto command=(down||e.is<events::controller_button_down>())?dialog_input::mapped(e.action):dialog_input::Command::none;
        switch(editor_.event(command,down,up)) {
        case dialog_input::Outcome::accept:return submit();
        case dialog_input::Outcome::cancel:if(cancel_)cancel_();return result::close|result::back_sound;
        default:return result::success;
        }
    }

    result tick(shell* xmb) override {
        if(!loaded_) {
            loaded_=true;
            for(const auto* name:{"osk/button_base_white","osk/button_base_blue_gray","osk/button_base_g","osk/osk_icon"}) icons_.load(*xmb->get_window()->loader,name);
        }
        return result::success;
    }
    void render(auroreengine::gui_renderer& r, shell* shell) override {
        xmb_canvas c(r);
        c.rect(0,0,1920,1080,{0,0,0,0.42F});
        c.rect(385,206,1149,668,{0,0,0,0.35F},12);
        const auto grey=[](float value) {const float v=std::pow((value+0.055F)/1.055F,2.4F);return glm::vec4(v,v,v,1);};
        const auto gradient=[&](float y,float h,float top,float bottom) {
            c.gradient(393,y,1133,h,grey(top),grey(bottom));
        };
        gradient(214,56,0.63F,0.56F);
        icons_.box(c,"osk/osk_icon",403,224,34,39);
        c.text(dialog_text::ellipsize(title_,1060*c.scale,[&](auto value){return r.text_width_pixels(value,28*c.scale*config::CONFIG.textScale);}),447,254,28);
        gradient(270,118,0.31F,0.14F);
        const float input_pixels=24*c.scale*config::CONFIG.textScale;
        const auto shown=dialog_text::tail(editor_.value,1090*c.scale,[&](auto value){return r.text_width_pixels(value,input_pixels);});
        c.text(shown,414,330,24);
        const float caret=414+r.text_width_pixels(shown,input_pixels)/c.scale;
        const auto metrics=r.text_line_metrics_pixels(input_pixels);
        const float caret_top=330-metrics.x/c.scale,caret_height=(metrics.x+metrics.y)/c.scale;
        const auto seconds=std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if(std::fmod(seconds,1.0F)<0.5F) {
            c.rect(caret+2,caret_top,2,caret_height,{0.43F,0.81F,0.18F,1});
            c.rect(caret,caret_top,6,2,{0.43F,0.81F,0.18F,1});c.rect(caret,caret_top+caret_height,6,2,{0.43F,0.81F,0.18F,1});
        }
        c.rect(393,388,1133,14,grey(0.61F));
        if(!config::CONFIG.enabled("input.osk")) {shell->render_controller_buttons(r,0.5F,0.78F,std::array{std::pair{action::options,"Save"},std::pair{action::extra,"Delete"},std::pair{action::cancel,"Back"}});return;}
        gradient(402,388,0.61F,0.38F);
        for(int row=0;row<5;++row) for(int col=0;col<10;++col) {
            const float x=418+col*109,y=418+row*57;
            const bool focused=row==editor_.row&&col==editor_.col;
            icons_.box(c,"osk/button_base_white",x,y,102,54,focused?1.0F:0.8F);
            if(focused) {c.rect(x+4,y+3,94,1,{1,1,1,0.9F});c.rect(x+4,y+50,94,1,{1,1,1,0.9F});}
            char ch=dialog_input::TextEntry::rows[row][col];if(editor_.upper()&&ch>='a'&&ch<='z') ch-='a'-'A';
            c.text(std::string(1,ch),x+76,y+39,27,{0.025F,0.025F,0.025F,1},0,true);
        }
        const std::array<std::string_view,4> labels{"Delete","Space","Shift","Done"};
        for(int i=0;i<4;++i) {
            const bool focused=editor_.row==5&&(editor_.col<2?0:editor_.col<5?1:editor_.col<7?2:3)==i;
            const float x=418+i*273;
            icons_.box(c,i==3?"osk/button_base_g":"osk/button_base_blue_gray",x,715,266,58,focused?1.0F:0.8F);
            c.text(labels[i],x+133,755,26,{0.025F,0.025F,0.025F,1},0,true);
            if(focused) c.rect(x+5,718,256,1,{1,1,1,1});
        }
        c.rect(393,790,1133,76,grey(0.38F));
        shell->render_controller_buttons(r,0.5F,0.78F,std::array{std::pair{action::options,"Save"},std::pair{action::extra,"Delete"},std::pair{action::cancel,"Back"}});
    }
    bool is_opaque() const override { return false; }
private:
    result submit() { accept_(editor_.value); return result::close | result::confirm_sound; }
    std::string title_;
    dialog_input::TextEntry editor_;
    std::function<void(std::string)> accept_;
    std::function<void()> cancel_;
    bool loaded_{};
    media_icons icons_;
};
}
