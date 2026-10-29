module;
#include "openxmb/setup/model.hpp"
#include "openxmb/setup/value_motion.hpp"
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
#include <tuple>
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
        if(const char* raw=std::getenv("OPENXMB_FIXED_VALUE_SECONDS")) {
            char* end=nullptr;const double value=std::strtod(raw,&end);
            if(end!=raw&&*end=='\0'&&value>=0&&std::isfinite(value)) fixed_value_age_=static_cast<float>(value);
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
        if(model_.page()==Page::date_time)date_value_=model_.current_time(now());
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
        if(model_.page()==Page::date_time)update_date(model_.current_time(now()),0);
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
        const auto prior_time=model_.current_time(now());
        const bool was_confirming=model_.confirmation();
        model_.command(*command,now());changed_page(page,reverse);
        if(page==Page::date_time&&model_.page()==Page::date_time) {
            if(!date_value_)date_value_=prior_time;
            update_date(model_.current_time(now()),input==action::up?1:input==action::down?-1:0);
        }
        if(!was_confirming&&model_.confirmation()) confirmation_opened_=Clock::now();
        if(page==Page::language&&old_language!=model_.draft().language&&host_) {
            config::CONFIG.setLanguage(model_.draft().language);
        }
        return result::success|result::ok_sound;
    }
    void render(auroreengine::gui_renderer& renderer,shell* host) override {
        renderer.draw_rect({0,0},{1,1},{0,0,0,1});
        xmb_canvas c(renderer);const auto page=model_.page();
        if(page==Page::brand||page==Page::company) { render_intro(c,page);return; }
        const float progress=std::clamp(elapsed()/0.25F,0.0F,1.0F);
        const float eased=dialog_chrome::smooth01(progress);
        if(previous_!=page&&progress<1) render_page(c,previous_,-direction_*530*eased,1-eased);
        render_page(c,page,previous_==page?0:direction_*530*(1-eased),previous_==page?1:eased);
        if(page!=Page::language&&!model_.editing()&&!model_.confirmation()) setup_controls::page_arrow(c,true);
        if(page!=Page::summary&&!model_.editing()&&!model_.confirmation()) setup_controls::page_arrow(c,false);
        if(model_.confirmation()) render_confirmation(c);
        // The header and rules stay bright above the confirmation shade.
        dialog_chrome::divider_frame(c,1);
        icons_.box(c,"theme/icon_setting_item",118,156,32,32,1);
        c.text("1·2·3",134,190,10,{1,1,1,0.9F},0,true);
        c.text("Initial Setup",163,188,26);
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
        if(old!=model_.page()) {
            previous_=old;direction_=reverse?-1.0F:1.0F;changed_=Clock::now();
            date_value_.reset();date_change_.reset();
        }
    }
    void update_date(openxmb::setup::CivilTime value,int direction) {
        if(date_value_&&*date_value_!=value) {
            if(direction==0) {
                const auto& old=*date_value_;
                direction=std::tie(value.year,value.month,value.day,value.hour,value.minute,value.second)<
                    std::tie(old.year,old.month,old.day,old.hour,old.minute,old.second)?-1:1;
            }
            date_change_=DateChange{*date_value_,Clock::now(),direction};
        }
        date_value_=value;
    }
    static std::array<std::string,11> date_parts(openxmb::setup::CivilTime t) {
        char day[8],month[8],year[12],hour[8],minute[8],second[8];
        std::snprintf(day,sizeof(day),"%02d",t.day);std::snprintf(month,sizeof(month),"%02d",t.month);
        std::snprintf(year,sizeof(year),"%04d",t.year);std::snprintf(hour,sizeof(hour),"%02d",t.hour);
        std::snprintf(minute,sizeof(minute),"%02d",t.minute);std::snprintf(second,sizeof(second),"%02d",t.second);
        return {day,"/",month,"/",year," ",hour,":",minute,":",second};
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
        const auto instruction=[&](std::string_view s){if(host_)host_->render_input_instruction(c.renderer,s,960+dx,866,22,alpha);};
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
                selectable_text(c,label,829+dx,450+40*static_cast<float>(row),26,i==index&&(language||!model_.daylight_focused()),alpha,false,false);
            }
            if(!language) {
                checkbox(c,830+dx,706,"Automatically adjust for daylight saving time",d.automatic_daylight,model_.daylight_focused(),alpha);
                if(model_.daylight_focused()&&d.zone_index<model_.zones().size()) {
                    const auto& zone=model_.zones()[d.zone_index];
                    if(!zone.rules_available) text(c,"Daylight saving rules are unavailable.",830+dx,765,20,false,alpha);
                    else if(!zone.observes_daylight) text(c,"This time zone does not observe daylight saving time.",830+dx,765,20,false,alpha);
                }
            }
            instruction(language?"Use {up} and {down} to select. Press {right} to continue.":
                model_.daylight_focused()?"Press {ok} to adjust daylight saving time. Press {extra} to return to the time zone list.":
                "Use {up} and {down} to select. Press {extra} to adjust daylight saving time. Press {right} to continue.");
        } else if(page==Page::date_time) {
            prompt("Set the time and date.");render_date(c,dx,alpha);
            if(!model_.editing()) checkbox(c,830+dx,650,"Use the system clock automatically",d.automatic_time,false,alpha);
            instruction(model_.editing()?"Use {up} and {down} to adjust the setting value and use {left} and {right} to select setting items. Press {ok} to accept your selections.":
                "Press {ok} to set the date and time. Press {extra} to set automatically. Press {right} to continue.");
        } else if(page==Page::user_name) {
            prompt("Enter your user name.");field(c,556+dx,520,808,40,d.user_name,alpha);
            instruction("Press {ok} to enter a user name. Press {right} to continue.");
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
        // The reference sets the line in fixed character cells, measured on
        // the 1080p editing still as digit 17 px, slash 13 px, colon
        // 8.5 px and a 12 px date/time gap; digits keep still as values
        // change. A wider glyph keeps its natural width.
        constexpr float size=28,baseline=549;
        const auto pieces=date_parts(model_.current_time(now()));
        const auto previous=date_change_?date_parts(date_change_->from):pieces;
        const auto motion=date_change_?openxmb::setup::value_motion(
            fixed_value_age_.value_or(std::chrono::duration<float>(Clock::now()-date_change_->started).count()),
            date_change_->direction,config::CONFIG.enabled("power.reduced.motion",false)):openxmb::setup::ValueMotion{};
        const float em=size*static_cast<float>(config::CONFIG.textScale);
        const auto cell=[&](char ch) {
            const float tabular=(ch=='/'?13.0F:ch==':'?8.5F:ch==' '?12.0F:17.0F)*em/size;
            return std::max(tabular,width(c,std::string_view(&ch,1),size,false));
        };
        const auto piece_width=[&](std::string_view piece) {float w=0;for(const char ch:piece) w+=cell(ch);return w;};
        float total=0;for(auto p:pieces) total+=piece_width(p);
        const float start=960+dx-total*0.5F;
        const bool editing=model_.editing();
        const auto active=[&](std::size_t i) {
            const int segment=i==0?0:i==2?1:i==4?2:i==6?3:i==8?4:-1;
            return editing?segment==model_.segment():true;
        };
        if(!editing) focus_glow(c,start,baseline,total,size,alpha);
        float x=start;
        for(std::size_t i=0;i<pieces.size();++i) {
            const float w=piece_width(pieces[i]);
            if(editing&&active(i)) {
                // Spinner marks sit ~6 px clear of the cap band, inside its light.
                focus_glow(c,x,baseline,w,size,alpha);
                spin_arrow(c,x+w*0.5F,baseline-cap_height(size)-6.5F,true,alpha);
                spin_arrow(c,x+w*0.5F,baseline+5.0F,false,alpha);
            }
            x+=w;
        }
        x=start;
        for(std::size_t i=0;i<pieces.size();++i) {
            for(std::size_t j=0;j<pieces[i].size();++j) {
                const char ch=pieces[i][j],old=previous[i][j];
                const float w=cell(ch);
                changing_text(c,std::string_view(&old,1),std::string_view(&ch,1),x+w*0.5F,baseline,size,motion,active(i),alpha);
                x+=w;
            }
        }
    }
    void render_confirmation(const xmb_canvas& c) const {
        using namespace setup_controls;
        // Traced on the 25 fps recording: within ~0.25 s the whole stage lifts
        // to ~0.04 sRGB and the page beneath falls to ~0.11 of its brightness,
        // while the header, rules and footer stay bright.
        const float fade=dialog_chrome::smooth01(fixed_intro_.value_or(dialog_chrome::seconds_since(confirmation_opened_))/
            std::chrono::duration<float>(dialog_chrome::message_fade).count());
        // Match both endpoints in the sRGB framebuffer, including its linear
        // toe: a gamma-2.2 approximation makes these near-black levels too dark.
        const float shade=1-(srgb_linear(0.11F)-srgb_linear(0.04F));
        const float lift=srgb_linear(0.04F)/shade;
        c.rect(0,0,1920,1080,{lift,lift,lift,shade*fade});
        text(c,"Finish initial setup?",348,478,26,false,fade);
        text(c,"Your choices will be saved for OpenXMB.",348,512,26,false,fade);
        selectable_text(c,"Yes",883,619,26,model_.confirmation_yes(),fade,true);
        selectable_text(c,"No",1040,619,26,!model_.confirmation_yes(),fade,true);
    }
    openxmb::setup::Model model_;
    std::function<void(const openxmb::setup::Draft&,const openxmb::setup::Zone&)> accept_;
    media_icons icons_;
    shell* host_{};
    Clock::time_point changed_{Clock::now()};
    Clock::time_point confirmation_opened_{Clock::now()};
    struct DateChange {openxmb::setup::CivilTime from;Clock::time_point started;int direction;};
    std::optional<openxmb::setup::CivilTime> date_value_;
    std::optional<DateChange> date_change_;
    std::optional<float> fixed_value_age_;
    Page previous_{Page::brand};
    float direction_{1};
    bool name_requested_{};
    std::optional<openxmb::setup::Instant> fixed_now_;
    std::optional<float> fixed_intro_;
    std::string error_;
};
}
