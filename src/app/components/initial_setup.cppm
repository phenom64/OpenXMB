module;
#include "openxmb/setup/model.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

export module openxmb.app:initial_setup;
import :component;
import :main;
import :xmb_presentation;
import :dialog_chrome;
import :setup_controls;
import :text_entry;
import auroreengine;
import glm;
import openxmb.config;
import openxmb.utils;

export namespace app {
class initial_setup_overlay final : public component, public action_receiver {
    using Page=openxmb::setup::Page;
    using Command=openxmb::setup::Command;
    using Clock=std::chrono::steady_clock;
public:
    explicit initial_setup_overlay(std::function<void(const openxmb::setup::Draft&,const openxmb::setup::Zone&)> accept)
        : accept_(std::move(accept)) {
        if(const char* raw=std::getenv("OPENXMB_FIXED_UNIX_SECONDS")) {
            char* end=nullptr;const auto value=std::strtoll(raw,&end,10);
            if(end!=raw&&*end=='\0') fixed_now_=openxmb::setup::Instant{std::chrono::seconds{value}};
        }
        if(const char* raw=std::getenv("OPENXMB_FIXED_SETUP_SECONDS")) {
            char* end=nullptr;const double value=std::strtod(raw,&end);
            if(end!=raw&&*end=='\0'&&value>=0&&std::isfinite(value)) fixed_intro_=static_cast<float>(value);
        }
        if(const char* raw=std::getenv("OPENXMB_SETUP_PAGE")) {
            const std::string_view name(raw);
            if(name=="brand") model_.show_page(Page::brand);
            else if(name=="company") model_.show_page(Page::company);
            else if(name=="language") model_.show_page(Page::language);
            else if(name=="time-zone") model_.show_page(Page::time_zone);
            else if(name=="date-time") model_.show_page(Page::date_time);
            else if(name=="name") model_.show_page(Page::user_name);
            else if(name=="summary"||name=="confirmation") {
                model_.show_page(Page::summary);
                if(name=="confirmation") model_.command(Command::confirm,now());
            }
        }
        if(const char* raw=std::getenv("OPENXMB_SETUP_EDIT");raw&&std::string_view(raw)=="1") {
            model_.show_page(Page::date_time);model_.command(Command::confirm,now());
        }
        previous_=model_.page();
    }
    void preload(auroreengine::resource_loader& loader,shell* host) {
        host_=host;
        icons_.load(loader,"branding/syndromatic");
        icons_.load(loader,"theme/icon_setting_item");
    }
    [[nodiscard]] bool is_opaque() const override { return true; }
    [[nodiscard]] bool do_fade_out() const override { return false; }
    result tick(shell* host) override {
        host_=host;
        const auto page=model_.page();
        if((page==Page::brand||page==Page::company)&&!fixed_intro_&&elapsed()>3.0F) {
            model_.advance_intro();changed_page(page);
        }
        if(name_requested_) {
            name_requested_=false;
            host->emplace_overlay<text_entry_overlay>("User Name",model_.draft().user_name,
                [this](std::string value){model_.set_name(std::move(value));},std::function<void()>{},keyboard_context::message);
        }
        if(model_.page()==Page::complete) {
            try {
                const auto& zones=model_.zones();
                if(zones.empty()||model_.draft().zone_index>=zones.size()) throw std::runtime_error("No time zone is available.");
                accept_(model_.draft(),zones[model_.draft().zone_index]);
                return result::close;
            } catch(const std::exception& e) {
                error_=e.what();model_.show_page(Page::summary);previous_=Page::summary;
            } catch(...) {
                error_="Could not save settings. Please try again.";model_.show_page(Page::summary);previous_=Page::summary;
            }
        }
        return result::success;
    }
    result on_action(action input) override {
        if(name_requested_) return result::success;
        const auto page=model_.page();
        if(page==Page::brand||page==Page::company) {
            if(input==action::ok) {model_.advance_intro();changed_page(page);}
            return result::success;
        }
        if(page==Page::user_name&&input==action::ok&&!model_.confirmation()) {
            name_requested_=true;return result::success|result::ok_sound;
        }
        std::optional<Command> command;
        switch(input) {
        case action::left:command=Command::left;break;
        case action::right:command=Command::right;break;
        case action::up:command=Command::up;break;
        case action::down:command=Command::down;break;
        case action::ok:command=Command::confirm;break;
        case action::cancel:command=Command::back;break;
        case action::extra:case action::options:command=Command::extra;break;
        default:return result::unsupported;
        }
        const bool reverse=input==action::left||input==action::cancel;
        if(!error_.empty()&&input==action::ok) error_.clear();
        const auto old_language=model_.draft().language;
        model_.command(*command,now());changed_page(page,reverse);
        if(page==Page::language&&old_language!=model_.draft().language&&host_) {
            config::CONFIG.setLanguage(model_.draft().language);
            host_->reload_language();
        }
        return result::success|result::ok_sound;
    }
    void render(auroreengine::gui_renderer& renderer,shell* host) override {
        renderer.draw_rect({0,0},{1,1},{0,0,0,1});
        xmb_canvas c(renderer);const auto page=model_.page();
        if(page==Page::brand||page==Page::company) { render_intro(c,page);return; }
        dialog_chrome::divider_frame(c,1);
        icons_.box(c,"theme/icon_setting_item",118,156,32,32,1);
        c.text("1·2·3",134,190,10,{1,1,1,0.9F},0,true);
        c.text("Initial Setup",163,188,26);
        const float progress=std::clamp(elapsed()/0.25F,0.0F,1.0F);
        const float eased=dialog_chrome::smooth01(progress);
        if(previous_!=page&&progress<1) render_page(c,previous_,-direction_*530*eased,1-eased);
        render_page(c,page,previous_==page?0:direction_*530*(1-eased),previous_==page?1:eased);
        if(page!=Page::language&&!model_.editing()&&!model_.confirmation()) setup_controls::page_arrow(c,true);
        if(page!=Page::summary&&!model_.editing()&&!model_.confirmation()) setup_controls::page_arrow(c,false);
        if(model_.confirmation()) render_confirmation(c);
        if(!error_.empty()) setup_controls::text(c,error_,960,765,20,false,1,true);
        if(!model_.error().empty()) setup_controls::text(c,model_.error(),960,795,20,false,1,true);
        if(model_.confirmation()||model_.editing())
            host->render_message_footer(renderer,std::array{std::pair{action::ok,"Enter"},std::pair{action::cancel,"Back"}},1);
        else if(page==Page::time_zone)
            host->render_message_footer(renderer,std::array{std::pair{action::extra,"Daylight saving"},std::pair{model_.daylight_focused()?action::ok:action::none,"Toggle"}},1);
        else if(page==Page::date_time)
            host->render_message_footer(renderer,std::array{std::pair{action::ok,"Edit"},std::pair{action::extra,"Automatic"}},1);
        else if(page==Page::user_name)
            host->render_message_footer(renderer,std::array{std::pair{action::ok,"Edit"}},1);
        else if(page==Page::summary)
            host->render_message_footer(renderer,std::array{std::pair{action::ok,"Enter"}},1);
    }
private:
    openxmb::setup::Instant now() const {return fixed_now_.value_or(std::chrono::system_clock::now());}
    float elapsed() const {return std::chrono::duration<float>(Clock::now()-changed_).count();}
    void changed_page(Page old,bool reverse=false) {
        if(old!=model_.page()) {previous_=old;direction_=reverse?-1.0F:1.0F;changed_=Clock::now();}
    }
    void render_intro(const xmb_canvas& c,Page page) const {
        const float t=fixed_intro_.value_or(elapsed());
        const float alpha=std::clamp(std::min(t/0.6F,(3.0F-t)/0.6F),0.0F,1.0F);
        if(page==Page::brand) icons_.draw(c,"branding/syndromatic",960,540,240,alpha);
        else setup_controls::text(c,"Syndromatic Limited Britannia",960,550,24,false,alpha,true);
    }
    void render_page(const xmb_canvas& c,Page page,float dx,float alpha) const {
        using namespace setup_controls;
        if(alpha<=0) return;
        const auto prompt=[&](std::string_view s){text(c,s,960+dx,325,26,false,alpha,true);};
        const auto instruction=[&](std::string_view s){text(c,s,960+dx,866,22,false,alpha,true);};
        const auto& d=model_.draft();
        if(page==Page::language||page==Page::time_zone) {
            const bool language=page==Page::language;
            prompt(language?"Select a language.":"Select a time zone.");
            const auto count=language?model_.languages().size():model_.zones().size();
            const auto index=language?d.language_index:d.zone_index;
            constexpr std::size_t visible=6;
            const auto first=count<=visible?std::size_t{0}:std::min(index>2?index-2:std::size_t{0},count-visible);
            scrollbar(c,800+dx,432,215,count,visible,first,alpha);
            for(std::size_t row=0;row<visible&&first+row<count;++row) {
                const auto i=first+row;
                const auto label=language?model_.languages()[i].label:model_.zones()[i].label();
                text(c,label,829+dx,450+40*static_cast<float>(row),26,i==index&&(language||!model_.daylight_focused()),alpha,false,false);
            }
            if(!language) {
                checkbox(c,830+dx,706,"Automatically adjust for daylight saving time",d.automatic_daylight,model_.daylight_focused(),alpha);
                if(model_.daylight_focused()&&d.zone_index<model_.zones().size()) {
                    const auto& zone=model_.zones()[d.zone_index];
                    if(!zone.rules_available) text(c,"Daylight saving rules are unavailable.",830+dx,765,20,false,alpha);
                    else if(!zone.observes_daylight) text(c,"This time zone does not observe daylight saving time.",830+dx,765,20,false,alpha);
                }
            }
            instruction(language?"Use up and down to select. Press right to continue.":"Select a time zone or adjust daylight saving. Press right to continue.");
        } else if(page==Page::date_time) {
            prompt("Set the time and date.");render_date(c,dx,alpha);
            if(!model_.editing()) checkbox(c,830+dx,650,"Use the system clock automatically",d.automatic_time,false,alpha);
            instruction(model_.editing()?"Use up and down to adjust values; left and right select fields.":"Edit the date and time or use the system clock. Press right to continue.");
        } else if(page==Page::user_name) {
            prompt("Enter your user name.");field(c,556+dx,520,808,40,d.user_name,alpha);
            instruction("Edit the user name, or press right to continue.");
        } else if(page==Page::summary||page==Page::complete) {
            prompt("Finish initial setup.");
            text(c,"Settings List",960+dx,474,26,false,alpha,true);
            const auto language=d.language_index<model_.languages().size()?model_.languages()[d.language_index].label:d.language;
            const auto zone=d.zone_index<model_.zones().size()?model_.zones()[d.zone_index].label():std::string{};
            const std::array<std::pair<std::string,std::string>,5> rows{{{"System Language",language},{"Time Zone",zone},{"Date and Time",openxmb::setup::format_time(model_.current_time(now()),true)},{"User Name",d.user_name},{"Region",d.region}}};
            for(std::size_t i=0;i<rows.size();++i) {
                const float y=508+40*static_cast<float>(i);
                text(c,rows[i].first,942+dx-width(c,rows[i].first,26),y,26,false,alpha);
                const float natural_width=width(c,rows[i].second,26,false);
                const float value_size=natural_width>832.0F?26.0F*832.0F/natural_width:26.0F;
                text(c,rows[i].second,978+dx,y,value_size,false,alpha,false,false);
            }
        }
    }
    void render_date(const xmb_canvas& c,float dx,float alpha) const {
        using namespace setup_controls;
        const auto t=model_.current_time(now());char day[8],month[8],year[12],hour[8],minute[8],second[8];
        std::snprintf(day,sizeof(day),"%02d",t.day);std::snprintf(month,sizeof(month),"%02d",t.month);
        std::snprintf(year,sizeof(year),"%04d",t.year);std::snprintf(hour,sizeof(hour),"%02d",t.hour);
        std::snprintf(minute,sizeof(minute),"%02d",t.minute);std::snprintf(second,sizeof(second),"%02d",t.second);
        const std::array<std::string_view,11> pieces{day,"/",month,"/",year," ",hour,":",minute,":",second};
        float total=0;for(auto p:pieces) total+=width(c,p,28,false);
        float x=960+dx-total*0.5F;
        for(std::size_t i=0;i<pieces.size();++i) {
            const float w=width(c,pieces[i],28,false);const int segment=i==0?0:i==2?1:i==4?2:i==6?3:i==8?4:-1;
            const bool active=model_.editing()?segment==model_.segment():true;
            text(c,pieces[i],x,549,28,active,alpha,false,false);
            if(model_.editing()&&active) {vertical_arrow(c,x+w*0.5F,521,true,alpha);vertical_arrow(c,x+w*0.5F,558,false,alpha);}
            x+=w;
        }
    }
    void render_confirmation(const xmb_canvas& c) const {
        using namespace setup_controls;
        c.rect(0,200,1920,679,{0,0,0,dialog_chrome::shade_alpha(0.94F)});
        text(c,"Finish initial setup?",348,478,26);
        text(c,"Your choices will be saved for OpenXMB.",348,512,26);
        text(c,"Yes",883,619,26,model_.confirmation_yes(),1,true);
        text(c,"No",1040,619,26,!model_.confirmation_yes(),1,true);
    }
    openxmb::setup::Model model_;
    std::function<void(const openxmb::setup::Draft&,const openxmb::setup::Zone&)> accept_;
    media_icons icons_;
    shell* host_{};
    Clock::time_point changed_{Clock::now()};
    Page previous_{Page::brand};
    float direction_{1};
    bool name_requested_{};
    std::optional<openxmb::setup::Instant> fixed_now_;
    std::optional<float> fixed_intro_;
    std::string error_;
};
}
