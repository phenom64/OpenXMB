/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025-2026 Syndromatic Ltd. All rights reserved
 * Designed by Kavish Krishnakumar in Manchester.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

module;

#include "dialog_text_layout.hpp"
#include "../../localization/shell_locale.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <chrono>
#include <optional>
#include <vector>
#include <cmath>

module openxmb.app;
import :message_overlay;

import auroreengine;
import :dialog_chrome;
import :main;
import :text_entry;
import :xmb_presentation;
import glm;
import spdlog;
import openxmb.config;

namespace app {
    message_overlay::message_overlay(std::string title, std::string message, std::vector<std::string> choices,
        std::function<void(unsigned int)> confirm_callback, bool cancelable, std::function<void()> cancel_callback
    ) : title(std::move(title)), message(std::move(message)), choices(std::move(choices)),
        confirm_callback(std::move(confirm_callback)), cancelable(cancelable), cancel_callback(std::move(cancel_callback))
    {
        this->title=openxmb::localization::shell_locale::translate(this->title);
        this->message=openxmb::localization::shell_locale::translate(this->message);
        for(auto& label:this->choices) label=openxmb::localization::shell_locale::translate(label);
    }

    void message_overlay::set_text_field(std::string keyboard_title, std::string value, std::function<void(std::string)> accept) {
        has_field=true;field_focused=true;
        this->keyboard_title=openxmb::localization::shell_locale::translate(keyboard_title);
        field_value=std::move(value);field_accept=std::move(accept);
        if(choices.empty()) choices.push_back(openxmb::localization::shell_locale::translate("OK"));
        selected=0;
    }

    std::chrono::milliseconds message_overlay::fade_duration() const {
        return dialog_chrome::message_fade;
    }

    void message_overlay::render(auroreengine::gui_renderer& renderer, shell* xmb) {
        if(choices.empty()) {
            selected = 0;
        } else {
            selected = std::min<unsigned int>(selected, static_cast<unsigned int>(choices.size() - 1));
        }

        // Staged entrance from the playlist capture (one partly dropped-frame
        // trial, ~0.25 s): footer hints first, then prompt, field and button,
        // and the dividers last, while the crossbar fades out underneath.
        const float age = dialog_chrome::seconds_since(start_time);
        const float footer_alpha = dialog_chrome::smooth01(age / 0.12F);
        const float content_alpha = dialog_chrome::smooth01((age - 0.03F) / 0.15F);
        const float divider_alpha = dialog_chrome::smooth01((age - 0.12F) / 0.13F);

        xmb_canvas c(renderer);
        c.rect(0,0,1920,1080,{0,0,0,dialog_chrome::shade_alpha(0.06F)*footer_alpha});
        dialog_chrome::divider_frame(c, divider_alpha);
        const float ui=config::CONFIG.textScale;
        const float pixels=24*c.scale*ui;
        const float title_pixels=dialog_chrome::prompt_size*c.scale*ui;
        const auto font=config::CONFIG.fontPath.string();
        if(wrapped_pixels!=pixels||wrapped_font!=font) {
            wrapped_pixels=pixels;wrapped_font=font;
            title_lines=dialog_text::wrap(title,1150*c.scale,[&](auto v){return renderer.text_width_pixels(v,title_pixels);});
            if(title_lines.size()>2) {title_lines.resize(2);title_lines.back()=dialog_text::ellipsize(title_lines.back()+"…",1150*c.scale,[&](auto v){return renderer.text_width_pixels(v,title_pixels);});}
            body_lines=dialog_text::wrap(message,1150*c.scale,[&](auto v){return renderer.text_width_pixels(v,pixels);});
        }
        const auto title_metrics=renderer.text_line_metrics_pixels(title_pixels)/c.scale;
        const float title_pitch=title_metrics.x+title_metrics.y+8*ui;
        const glm::vec4 white{1,1,1,content_alpha};
        const float press=dialog_chrome::press_amount(pressed_at);
        if(has_field) {
            for(std::size_t i=0;i<title_lines.size();++i)
                c.text(title_lines[i],960,dialog_chrome::prompt_baseline_y+i*title_pitch,dialog_chrome::prompt_size,white,0,true);
            dialog_chrome::text_field(c,renderer,dialog_chrome::field_x,dialog_chrome::field_y,dialog_chrome::field_width,dialog_chrome::field_height,
                field_value,{.focus=field_focused?1.0F:0.0F,.pressed=field_focused?press:0.0F,.alpha=content_alpha});
            dialog_chrome::push_button_row(c,renderer,960,dialog_chrome::field_y+dialog_chrome::field_height+dialog_chrome::field_to_button_gap,
                choices,field_focused?choices.size():selected,field_focused?choices.size():selected,press,content_alpha);
        } else {
            const auto body_metrics=renderer.text_line_metrics_pixels(pixels)/c.scale;
            const auto button_metrics=renderer.text_line_metrics_pixels(dialog_chrome::button_text_size*c.scale*ui)/c.scale;
            const auto page=dialog_text::message_layout(title_lines.size(),body_lines.size(),choices.size(),
                {title_metrics.x,title_metrics.y},{body_metrics.x,body_metrics.y},{button_metrics.x,button_metrics.y},ui);
            for(std::size_t i=0;i<title_lines.size();++i)
                c.text(title_lines[i],960,page.title_last_baseline-(title_lines.size()-1-i)*title_pitch,dialog_chrome::prompt_size,white,0,true);
            line_count=body_lines.size();visible_lines=page.capacity;
            scroll_line=std::min(scroll_line,line_count>visible_lines?line_count-visible_lines:0);
            for(std::size_t i=0;i<visible_lines&&i+scroll_line<line_count;++i)
                c.text(body_lines[i+scroll_line],960,page.body_first_baseline+i*page.body_pitch,24,white,0,true);
            if(!choices.empty())
                dialog_chrome::push_button_row(c,renderer,960,page.choice_first_baseline-dialog_chrome::button_height+12,
                    choices,selected,selected,press,content_alpha);
            if(line_count>visible_lines) {
                if(scroll_line)c.text("▲",1540,page.body_top+body_metrics.x,20,white);
                if(scroll_line+visible_lines<line_count)c.text("▼",1540,page.body_bottom,20,white);
            }
        }
        const auto edit_or_enter=has_field&&field_focused?"Edit":"Enter";
        xmb->render_message_footer(renderer,std::array{
            std::pair{action::ok,edit_or_enter},std::pair{cancelable?action::cancel:action::none,"Back"}},footer_alpha);
    }

    result message_overlay::tick(shell* xmb) {
        if(!pending||dialog_chrome::seconds_since(pressed_at)<std::chrono::duration<float>(dialog_chrome::press_feedback).count())
            return result::success;
        pending=false;
        if(has_field&&field_focused) {
            xmb->emplace_overlay<text_entry_overlay>(keyboard_title,field_value,
                [this](std::string value){field_value=std::move(value);},std::function<void()>{},keyboard_context::message);
            return result::success | result::confirm_sound;
        }
        if(has_field) {
            if(field_accept) field_accept(field_value);
        } else if(confirm_callback) {
            confirm_callback(selected);
        }
        return result::success | result::close | result::confirm_sound;
    }

    result message_overlay::on_action(action action) {
        if(pending) return result::success;
        switch(action) {
            case action::up:
                if(has_field) {
                    if(field_focused) return result::success | result::error_rumble;
                    field_focused=true;return result::success | result::ok_sound;
                }
                if(scroll_line) --scroll_line;
                return result::success;
            case action::down:
                if(has_field) {
                    if(!field_focused) return result::success | result::error_rumble;
                    field_focused=false;selected=0;return result::success | result::ok_sound;
                }
                if(scroll_line+visible_lines<line_count) ++scroll_line;
                return result::success;
            case action::cancel:
                if(cancelable) {
                    if(cancel_callback) {
                        cancel_callback();
                    }
                    return result::success | result::close | result::cancel_sound;
                }
                return result::success;
            case action::ok:
                pending=true;pressed_at=std::chrono::steady_clock::now();
                return result::success;
            case action::left:
                if(choices.empty() || (has_field && field_focused)) {
                    return result::success | result::error_rumble;
                }
                if(selected > 0) {
                    selected--;
                    return result::success | result::ok_sound;
                }
                return result::success | result::error_rumble;
            case action::right:
                if(choices.empty() || (has_field && field_focused)) {
                    return result::success | result::error_rumble;
                }
                if(selected + 1 < choices.size()) {
                    selected++;
                    return result::success | result::ok_sound;
                }
                return result::success | result::error_rumble;
            default:
                return result::success;
        }
    }
}
