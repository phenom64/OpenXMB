module;

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
import auroreengine;
import glm;

export namespace app {

class text_entry_overlay : public component, public event_receiver {
public:
    float typography_scale() const override { return 2.5F; }

    text_entry_overlay(std::string title, std::string initial,
                       std::function<void(std::string)> accept,std::function<void()> cancel={})
        : title_(std::move(title)), value_(std::move(initial)), accept_(std::move(accept)),cancel_(std::move(cancel)) {}

    result on_event(const event& e) override {
        if (const auto* key = e.get<events::key_up>(); key && (key->keycode==225 || key->keycode==229)) { upper_=false; return result::success; }
        if (const auto* key = e.get<events::key_down>()) {
            const unsigned code = key->keycode;
            if (code == 40 || code == 88) return submit();
            if (code == 42) { erase(); return result::success; }
            if (code == 225 || code == 229) { upper_ = true; return result::success; }
            char ch = 0;
            if (code >= 4 && code <= 29) ch = static_cast<char>((upper_ ? 'A' : 'a') + code - 4);
            else if (code >= 30 && code <= 38) ch = upper_ ? std::string_view("!@#$%^&*(")[code-30] : static_cast<char>('1' + code - 30);
            else if (code == 39) ch = upper_ ? ')' : '0';
            else if (code == 44) ch = ' ';
            else if (code >= 45 && code <= 56) {
                constexpr std::array<char, 12> punctuation{'-', '=', '[', ']', '\\', '\\', ';', '\'', '`', ',', '.', '/'};
                constexpr std::string_view shifted = "_+{}||:\"~<>?";
                ch = upper_ ? shifted[code-45] : punctuation[code - 45];
            }
            if (ch) { if (value_.size() < 4096) value_ += ch; return result::success; }
        }
        switch (e.action) {
        case action::cancel: if(cancel_) cancel_();return result::close | result::back_sound;
        case action::up: row_ = (row_ + 5) % 6; break;
        case action::down: row_ = (row_ + 1) % 6; break;
        case action::left: col_ = (col_ + 9) % 10; break;
        case action::right: col_ = (col_ + 1) % 10; break;
        case action::extra: erase(); break;
        case action::options: return submit();
        case action::ok:
            if (row_ == 5) {
                if (col_ < 2) erase();
                else if (col_ < 5) { if (value_.size() < 4096) value_ += ' '; }
                else if (col_ < 7) upper_ = !upper_;
                else return submit();
            } else if (value_.size() < 4096) {
                char ch = rows_[row_][col_];
                if (upper_ && ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
                value_ += ch;
            }
            break;
        default: return result::success;
        }
        return result::success;
    }

    result tick(shell* xmb) override {
        if(!loaded_) {
            loaded_=true;
            for(const auto* name:{"osk/button_base_white","osk/button_base_blue_gray","osk/button_base_g","osk/osk_icon"}) icons_.load(*xmb->get_window()->loader,name);
        }
        return result::success;
    }
    void render(auroreengine::gui_renderer& r, shell*) override {
        xmb_canvas c(r);
        c.rect(0,0,1920,1080,{0,0,0,0.42F});
        c.rect(385,206,1149,668,{0,0,0,0.35F},12);
        const auto grey=[](float value) {const float v=std::pow((value+0.055F)/1.055F,2.4F);return glm::vec4(v,v,v,1);};
        const auto gradient=[&](float y,float h,float top,float bottom) {
            c.gradient(393,y,1133,h,grey(top),grey(bottom));
        };
        gradient(214,56,0.63F,0.56F);
        icons_.box(c,"osk/osk_icon",403,224,34,39);
        c.text(title_,447,254,28);
        gradient(270,118,0.31F,0.14F);
        const auto shown=value_.size()>83?"..."+value_.substr(value_.size()-83):value_;
        c.text(shown,414,312,24);
        const float caret=414+r.text_width_pixels(shown,24*c.scale)/c.scale;
        const auto seconds=std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if(std::fmod(seconds,1.0F)<0.5F) {
            c.rect(caret+2,284,2,34,{0.43F,0.81F,0.18F,1});
            c.rect(caret,284,6,2,{0.43F,0.81F,0.18F,1});c.rect(caret,316,6,2,{0.43F,0.81F,0.18F,1});
        }
        c.rect(393,388,1133,14,grey(0.61F));
        gradient(402,388,0.61F,0.38F);
        for(int row=0;row<5;++row) for(int col=0;col<10;++col) {
            const float x=418+col*109,y=418+row*57;
            const bool focused=row==row_&&col==col_;
            icons_.box(c,"osk/button_base_white",x,y,102,54,focused?1.0F:0.8F);
            if(focused) {c.rect(x+4,y+3,94,1,{1,1,1,0.9F});c.rect(x+4,y+50,94,1,{1,1,1,0.9F});}
            char ch=rows_[row][col];if(upper_&&ch>='a'&&ch<='z') ch-='a'-'A';
            c.text(std::string(1,ch),x+76,y+39,27,{0.025F,0.025F,0.025F,1},0,true);
        }
        const std::array<std::string_view,4> labels{"Delete","Space","Shift","Done"};
        for(int i=0;i<4;++i) {
            const bool focused=row_==5&&(col_<2?0:col_<5?1:col_<7?2:3)==i;
            const float x=418+i*273;
            icons_.box(c,i==3?"osk/button_base_g":"osk/button_base_blue_gray",x,715,266,58,focused?1.0F:0.8F);
            c.text(labels[i],x+133,755,26,{0.025F,0.025F,0.025F,1},0,true);
            if(focused) c.rect(x+5,718,256,1,{1,1,1,1});
        }
        c.rect(393,790,1133,76,grey(0.38F));
        c.text("Enter / Options: save    Cancel: back    Extra: delete",960,838,23,{1,1,1,0.95F},0,true);
    }
    bool is_opaque() const override { return false; }
private:
    void erase() { if (!value_.empty()) { auto start=value_.size()-1; while(start>0 && (static_cast<unsigned char>(value_[start]) & 0xc0)==0x80) --start; value_.resize(start); } }
    result submit() { accept_(value_); return result::close | result::confirm_sound; }
    static constexpr std::array<std::string_view, 5> rows_{"1234567890", "qwertyuiop", "asdfghjkl:", "zxcvbnm./-", "_@+?=\\()[]"};
    std::string title_, value_;
    std::function<void(std::string)> accept_;
    std::function<void()> cancel_;
    int row_ = 0, col_ = 0;
    bool upper_ = false;
    bool loaded_{};
    media_icons icons_;
};
}
