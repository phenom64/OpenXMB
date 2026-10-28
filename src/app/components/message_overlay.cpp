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
#include <cmath>

module openxmb.app;
import :message_overlay;

import auroreengine;
import :xmb_presentation;
import glm;
import spdlog;
import openxmb.config;

namespace app {
    namespace {
        float smooth01(float value) {
            value = std::clamp(value, 0.0f, 1.0f);
            return value * value * (3.0f - 2.0f * value);
        }
    }

    message_overlay::message_overlay(std::string title, std::string message, std::vector<std::string> choices,
        std::function<void(unsigned int)> confirm_callback, bool cancelable, std::function<void()> cancel_callback
    ) : title(std::move(title)), message(std::move(message)), choices(std::move(choices)),
        confirm_callback(std::move(confirm_callback)), cancelable(cancelable), cancel_callback(std::move(cancel_callback))
    {
        this->title=openxmb::localization::shell_locale::translate(this->title);
        this->message=openxmb::localization::shell_locale::translate(this->message);
        for(auto& label:this->choices) label=openxmb::localization::shell_locale::translate(label);
    }

    void message_overlay::render(auroreengine::gui_renderer& renderer, shell* xmb) {
        if(choices.empty()) {
            selected = 0;
        } else {
            selected = std::min<unsigned int>(selected, static_cast<unsigned int>(choices.size() - 1));
        }

        using clock = std::chrono::steady_clock;
        float age = std::chrono::duration<float>(clock::now() - start_time).count();
        float settle = smooth01(age / 0.28f);
        renderer.push_color(glm::vec4(1.0f, 1.0f, 1.0f, settle));

        xmb_canvas c(renderer);
        c.rect(0,0,1920,1080,{0,0,0,0.25F});
        c.rect(0,199,1920,680,{0,0,0,0.12F});
        c.rect(0,199,1920,1,{1,1,1,0.75F});c.rect(0,879,1920,1,{1,1,1,0.75F});
        const float ui=config::CONFIG.textScale;
        const float pixels=24*c.scale*ui;
        const auto font=config::CONFIG.fontPath.string();
        if(wrapped_pixels!=pixels||wrapped_font!=font) {
            wrapped_pixels=pixels;wrapped_font=font;
            title_lines=dialog_text::wrap(title,1150*c.scale,[&](auto v){return renderer.text_width_pixels(v,28*c.scale*ui);});
            if(title_lines.size()>2) {title_lines.resize(2);title_lines.back()=dialog_text::ellipsize(title_lines.back()+"…",1150*c.scale,[&](auto v){return renderer.text_width_pixels(v,28*c.scale*ui);});}
            body_lines=dialog_text::wrap(message,1150*c.scale,[&](auto v){return renderer.text_width_pixels(v,pixels);});
            choice_lines.clear();
            const float width=1320*c.scale/std::max(std::size_t{1},choices.size())-40*c.scale;
            for(const auto& choice:choices) {
                auto lines=dialog_text::wrap(choice,width,[&](auto v){return renderer.text_width_pixels(v,26*c.scale*ui);});
                if(lines.size()>2){lines.resize(2);lines.back()=dialog_text::ellipsize(lines.back()+"…",width,[&](auto v){return renderer.text_width_pixels(v,26*c.scale*ui);});}
                choice_lines.push_back(std::move(lines));
            }
        }
        const auto title_metrics=renderer.text_line_metrics_pixels(28*c.scale*ui)/c.scale;
        const auto body_metrics=renderer.text_line_metrics_pixels(pixels)/c.scale;
        const auto choice_metrics=renderer.text_line_metrics_pixels(26*c.scale*ui)/c.scale;
        const auto page=dialog_text::message_layout(title_lines.size(),body_lines.size(),choices.size(),
            {title_metrics.x,title_metrics.y},{body_metrics.x,body_metrics.y},{choice_metrics.x,choice_metrics.y},ui);
        const float title_pitch=title_metrics.x+title_metrics.y+8*ui;
        for(std::size_t i=0;i<title_lines.size();++i)c.text(title_lines[i],960,page.title_last_baseline-(title_lines.size()-1-i)*title_pitch,28,{1,1,1,1},0,true);
        line_count=body_lines.size();visible_lines=page.capacity;
        scroll_line=std::min(scroll_line,line_count>visible_lines?line_count-visible_lines:0);
        for(std::size_t i=0;i<visible_lines&&i+scroll_line<line_count;++i)
            c.text(body_lines[i+scroll_line],960,page.body_first_baseline+i*page.body_pitch,24,{1,1,1,1},0,true);
        const float column=1320.0F/std::max(std::size_t{1},choices.size());
        for(std::size_t i=0;i<choices.size();++i)for(std::size_t line=0;line<choice_lines[i].size();++line)
            c.text(choice_lines[i][line],300+column*(i+0.5F),page.choice_first_baseline+line*(choice_metrics.x+choice_metrics.y+8*ui),26,{1,1,1,1},i==selected?xmb_canvas::pulse():0,true);
        if(line_count>visible_lines) {
            if(scroll_line)c.text("▲",1540,page.body_top+body_metrics.x,20);
            if(scroll_line+visible_lines<line_count)c.text("▼",1540,page.body_bottom,20);
        }
        const auto previous_scale=renderer.text_scale;renderer.text_scale=1.0F;
        xmb->render_controller_buttons(renderer,0.5F,0.84F,std::array{
            std::pair{action::ok,"Enter"},std::pair{cancelable?action::cancel:action::none,"Back"}});
        renderer.text_scale=previous_scale;
        renderer.pop_color();
    }

    result message_overlay::on_action(action action) {
        switch(action) {
            case action::up:
                if(scroll_line) --scroll_line;
                return result::success;
            case action::down:
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
                if(confirm_callback) {
                    confirm_callback(selected);
                }
                return result::success | result::close | result::confirm_sound;
            case action::left:
                if(choices.empty()) {
                    return result::success | result::error_rumble;
                }
                if(selected > 0) {
                    selected--;
                    return result::success | result::ok_sound;
                }
                return result::success | result::error_rumble;
            case action::right:
                if(choices.empty()) {
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
