module;
#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <stdexcept>
export module openxmb.app:settings_center;
import :component;
import :main;
import :text_entry;
import :message_overlay;
import openxmb.config;
import openxmb.utils;
import dreamrender;
import glm;
import vulkan_hpp;

export namespace programs {
class settings_center : public app::component, public action_receiver {
public:
    float typography_scale() const override { return 2.5F; }

    explicit settings_center(dreamrender::resource_loader&) {}
    result tick(app::shell* shell) override { shell_ = shell; return result::success; }
    result on_action(action a) override {
        if(a == action::cancel) { if(input_) { input_ = false; selection_ = 0; return result::success; } return result::close; }
        const int count = input_ ? 10 : 23;
        if(a == action::up) selection_ = (selection_ + count - 1) % count;
        if(a == action::down) selection_ = (selection_ + 1) % count;
        if(a != action::ok || !shell_) return result::success;
        auto& cfg = config::CONFIG;
        if(input_) {
            if(selection_ == 0) { input_ = false; selection_ = 0; return result::success; }
            if(selection_ == 9) { for(const auto& key : keys_) cfg.preferences.erase("key." + std::string(key)); cfg.save_config(); return result::success; }
            const auto key = std::string(keys_[selection_-1]);
            shell_->emplace_overlay<app::text_entry_overlay>("SDL scancode for " + key,
                cfg.preferences.contains("key."+key) ? cfg.preferences["key."+key] : std::to_string(defaults_[selection_-1]),
                [this,key](std::string text) { try { std::size_t used{}; const auto code=std::stoi(text,&used); if(used!=text.size() || code<4 || code>512) throw std::runtime_error("Use an SDL scancode from 4 to 512"); config::CONFIG.preferences["key."+key]=text; config::CONFIG.save_config(); message_="Binding saved"; } catch(const std::exception& error) { message_=error.what(); } });
            return result::success;
        }
        static const std::array<std::string,9> panels{"themes","users","custom","games","browser","emulation","discs","system","account"};
        if(selection_ < 9) shell_->open_panel(panels[selection_]);
        else if(selection_ == 9) {
            const auto samples = static_cast<int>(cfg.sampleCount);
            cfg.setSampleCount(static_cast<vk::SampleCountFlagBits>(samples >= 8 ? 1 : samples*2));
            shell_->get_window()->waitIdle(); shell_->get_window()->config.sampleCount=cfg.sampleCount; shell_->get_window()->recreateSwapchain();
        } else if(selection_ == 10) {
            cfg.setMaxFPS(cfg.maxFPS < 31 ? 60 : cfg.maxFPS < 61 ? 120 : cfg.maxFPS < 121 ? 0 : 30);
            shell_->get_window()->config.fpsLimit=cfg.maxFPS > 10000 ? 0 : cfg.maxFPS;
        } else if(selection_ == 11) cfg.setVSync(cfg.preferredPresentMode == vk::PresentModeKHR::eImmediate);
        else if(selection_ == 12) { cfg.setIconGlassRefraction(!cfg.iconGlassRefraction); shell_->reload_menu(); }
        else if(selection_ == 13) { input_=true; selection_=0; }
        else if(selection_ == 14) {
            const std::array<std::string,5> types{"auto","keyboard","playstation","xbox","steam"};
            auto it=std::find(types.begin(),types.end(),cfg.controllerType); cfg.setControllerType(types[(it==types.end()?0:std::distance(types.begin(),it)+1)%types.size()]);
        } else if(selection_ == 15) cfg.setControllerRumble(!cfg.controllerRumble);
        else if(selection_ == 16) cfg.setShowFPS(!cfg.showFPS);
        else if(selection_ == 17) cfg.setShowMemory(!cfg.showMemory);
        else if(selection_ >=18 && selection_ <=21) {
            const int field=selection_;
            const auto initial=field==18?cfg.picturesPath:field==19?cfg.musicPath:field==20?cfg.videosPath:cfg.fontPath;
            shell_->emplace_overlay<app::text_entry_overlay>(field==21?"Font file":"Media folder",initial.string(),[this,field](std::string path){
                try {
                    if(field==21) { config::CONFIG.setFontPath(path); shell_->reload_fonts(); }
                    else { if(!std::filesystem::is_directory(path)) throw std::runtime_error("Folder does not exist"); if(field==18) config::CONFIG.picturesPath=path; if(field==19) config::CONFIG.musicPath=path; if(field==20) config::CONFIG.videosPath=path; shell_->reload_menu(); }
                    config::CONFIG.save_config(); message_="Saved";
                } catch(const std::exception& error) { message_=error.what(); }
            });
        } else if(selection_ == 22) {
            shell_->emplace_overlay<app::message_overlay>("Reset settings?", "Restore the current profile's default shell preferences? Media files and other profiles are preserved.",std::vector<std::string>{"Cancel","Reset"},[this](unsigned index){ if(index) { config::CONFIG.reset_to_defaults(); shell_->reload_fonts(); shell_->reload_menu(); message_="Defaults restored"; } });
        }
        cfg.save_config();
        return result::success | result::ok_sound;
    }
    void render(dreamrender::gui_renderer& r, app::shell* shell) override {
        shell_=shell;
        auto& c=config::CONFIG;
        r.draw_rect({0.06F,0.07F},{0.88F,0.86F},{0.018F,0.026F,0.045F,0.97F});
        r.draw_text(input_?"Keyboard Controls":"OpenXMB Settings",0.10F,0.14F,0.05F);
        std::vector<std::string> rows;
        if(input_) {
            rows.push_back("Back");
            for(std::size_t i=0;i<keys_.size();++i) rows.push_back(std::string(keys_[i])+"    "+(c.preferences.contains("key."+std::string(keys_[i]))?c.preferences["key."+std::string(keys_[i])]:std::to_string(defaults_[i])));
            rows.push_back("Restore default bindings");
        } else {
            rows={"Theme Library","Local Profiles","Custom Sections","Game Library","Internet Browser","Emulation","Disc Media","System Tools","Local Account",
                "Anti-aliasing    "+std::to_string(static_cast<int>(c.sampleCount))+"x", "Frame cap    "+(c.maxFPS>10000?"Unlimited":std::to_string(static_cast<int>(c.maxFPS))),
                "VSync    "+std::string(c.preferredPresentMode==vk::PresentModeKHR::eImmediate?"Off":"On"), "Glass icons    "+std::string(c.iconGlassRefraction?"On":"Off"),
                "Keyboard bindings","Controller prompts    "+c.controllerType,"Controller rumble    "+std::string(c.controllerRumble?"On":"Off"),
                "Show FPS    "+std::string(c.showFPS?"On":"Off"),"Show GPU memory    "+std::string(c.showMemory?"On":"Off"),"Pictures folder","Music folder","Videos folder","Font file","Reset current profile settings"};
        }
        const auto first=selection_>=10?selection_-9:0;
        for(int i=first;i<std::min<int>(rows.size(),first+10);++i) {
            const float y=0.25F+(i-first)*0.053F;
            if(i==selection_) r.draw_rect({0.085F,y-0.025F},{0.83F,0.049F},{0.15F,0.27F,0.42F,0.85F});
            r.draw_text(rows[i],0.11F,y,0.032F);
        }
        r.draw_text(message_,0.10F,0.82F,0.025F);
        r.draw_text("Up/Down: select    Confirm: change    Cancel: back",0.10F,0.88F,0.025F);
    }
    bool is_opaque() const override { return false; }
private:
    static constexpr std::array<std::string_view,8> keys_{"left","right","up","down","ok","cancel","options","extra"};
    static constexpr std::array<int,8> defaults_{80,79,82,81,40,41,43,57};
    app::shell* shell_{};
    int selection_{};
    bool input_{};
    std::string message_;
};
}
