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
#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

module openxmb.app;
import :choice_overlay;
import :xmb_presentation;
import openxmb.config;

import auroreengine;
import glm;
import openxmb.utils;
import vulkan_hpp;
import vma;

namespace app {

choice_overlay::choice_overlay(std::vector<std::string> choices, unsigned int selection_index,
    std::function<void(unsigned int)> confirm_callback,
    std::function<void()> cancel_callback,
    std::function<void(unsigned int)> preview_callback)
    : choices{std::move(choices)}, confirm_callback{std::move(confirm_callback)},
      cancel_callback{std::move(cancel_callback)},
      preview_callback{std::move(preview_callback)}
{
    if(this->choices.empty()) {
        this->selection_index = 0;
        this->last_selection_index = 0;
    } else {
        this->selection_index = std::min<unsigned int>(selection_index, static_cast<unsigned int>(this->choices.size() - 1));
        this->last_selection_index = this->selection_index;
    }
}

result choice_overlay::on_action(action action) {
    switch(action) {
        case action::cancel:
            if(cancel_callback) {
                cancel_callback();
            }
            return result::close | result::back_sound;
        case action::ok:
            if(choices.empty()) {
                return result::success | result::error_rumble;
            }
            if(confirm_callback) {
                confirm_callback(selection_index);
            }
            return result::close | result::confirm_sound;
        case action::left:
            text_scroll_line=text_scroll_line>selected_page_lines?text_scroll_line-selected_page_lines:0;
            return result::success;
        case action::right:
            if(selected_total_lines>selected_page_lines)text_scroll_line=std::min(text_scroll_line+selected_page_lines,selected_total_lines-selected_page_lines);
            return result::success;
        case action::up:
            return select_relative(action::up) ? result::success | result::ok_sound : result::success | result::error_rumble;
        case action::down:
            return select_relative(action::down) ? result::success | result::ok_sound : result::success | result::error_rumble;
        default:
            return result::success | result::error_rumble;
    }
}

bool choice_overlay::select_relative(action dir) {
    if(choices.empty()) {
        return false;
    }

    if(dir == action::up) {
        if(selection_index <= 0) {
            return false;
        }
        last_selection_index = selection_index;
        last_selection_time = std::chrono::system_clock::now();
        selection_index = (selection_index + choices.size() - 1) % choices.size();
    } else if(dir == action::down) {
        if(selection_index >= choices.size() - 1) {
            return false;
        }
        last_selection_index = selection_index;
        last_selection_time = std::chrono::system_clock::now();
        selection_index = (selection_index + 1) % choices.size();
    } else {
        return false;
    }
    text_scroll_line=0;
    if(preview_callback) {
        preview_callback(selection_index);
    }
    return true;
}

void choice_overlay::render(auroreengine::gui_renderer& renderer, class shell* xmb) {
    (void)xmb;

    struct gradient_stop {
        float position;
        glm::vec4 colour;
    };

    // xmb-web's firmware-measured Theme/Colour/Background side panel is a
    // right-side translucent lavender wash, not a recoloured copy of the active
    // month.  Keep the XMB visible behind it and interpolate the measured
    // multi-stop profile with small quads.
    const auto open_elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - opened_time);
    const auto open_linear = std::clamp(
        open_elapsed / std::chrono::duration<double>(open_duration), 0.0, 1.0);
    const auto open_alpha =
        static_cast<float>(open_linear * open_linear * (3.0 - 2.0 * open_linear));
    const float x_shift = (1.0F - open_alpha) * (12.0F / 1920.0F);
    constexpr float panel_native_alpha_scale = 0.90F;
    constexpr float panel_left = 1324.0F / 1920.0F;
    constexpr float panel_right = 1697.0F / 1920.0F;
    constexpr std::array<gradient_stop, 8> stops{{
        {0.000F, {178.0F / 255.0F, 170.0F / 255.0F, 194.0F / 255.0F, 0.00F}},
        {0.043F, {178.0F / 255.0F, 170.0F / 255.0F, 194.0F / 255.0F, 0.55F}},
        {0.142F, {151.0F / 255.0F, 141.0F / 255.0F, 174.0F / 255.0F, 0.88F}},
        {0.300F, {160.0F / 255.0F, 150.0F / 255.0F, 182.0F / 255.0F, 0.80F}},
        {0.470F, {174.0F / 255.0F, 165.0F / 255.0F, 191.0F / 255.0F, 0.65F}},
        {0.651F, {195.0F / 255.0F, 189.0F / 255.0F, 210.0F / 255.0F, 0.45F}},
        {0.820F, {220.0F / 255.0F, 217.0F / 255.0F, 229.0F / 255.0F, 0.22F}},
        {1.000F, {245.0F / 255.0F, 244.0F / 255.0F, 247.0F / 255.0F, 0.00F}},
    }}; 
    for(std::size_t i = 0; i + 1 < stops.size(); ++i) {
        const float x0 = panel_left + x_shift + (panel_right - panel_left) * stops[i].position;
        const float x1 = panel_left + x_shift + (panel_right - panel_left) * stops[i + 1].position;
        auto c0 = stops[i].colour;
        auto c1 = stops[i + 1].colour;
        c0.a *= open_alpha * panel_native_alpha_scale;
        c1.a *= open_alpha * panel_native_alpha_scale;
        renderer.draw_quad(std::array{
            auroreengine::simple_renderer::vertex_data{{x0, 0.0f}, c0, {0.0f, 0.0f}},
            auroreengine::simple_renderer::vertex_data{{x0, 1.0f}, c0, {0.0f, 1.0f}},
            auroreengine::simple_renderer::vertex_data{{x1, 0.0f}, c1, {1.0f, 0.0f}},
            auroreengine::simple_renderer::vertex_data{{x1, 1.0f}, c1, {1.0f, 1.0f}},
        }, auroreengine::simple_renderer::params{});
    }

    auto now = std::chrono::system_clock::now();
    double selected = selection_index;
    auto time_since_transition = std::chrono::duration<double>(now - last_selection_time);
    if(time_since_transition < transition_duration) {
        double p = std::clamp(time_since_transition / transition_duration, 0.0, 1.0);
        p = p * p * (3.0 - 2.0 * p);
        selected = last_selection_index + (selected - last_selection_index) * p;
    }

    xmb_canvas canvas(renderer);
    const float ui=config::CONFIG.textScale;
    const float pixels=28*canvas.scale*ui;
    const auto font=config::CONFIG.fontPath.string();
    const auto metrics=renderer.text_line_metrics_pixels(pixels)/canvas.scale;
    const float line_height=metrics.x+metrics.y,pitch=line_height+8*ui;
    constexpr float available_width=337;
    if(wrapped_pixels!=pixels||wrapped_font!=font) {
        wrapped_pixels=pixels;wrapped_font=font;
        wrapped_choices.clear();row_heights.clear();
        for(const auto& entry:choices) {
            auto lines=dialog_text::wrap(entry,available_width*canvas.scale,[&](auto v){return renderer.text_width_pixels(v,pixels);});
            row_heights.push_back(line_height+(lines.size()-1)*pitch+12*ui);
            wrapped_choices.push_back(std::move(lines));
        }
    }
    const float x=1340+(1-open_alpha)*12;
    float top=518-metrics.x;
    if(!top_action.empty()) {
        auto labels=dialog_text::wrap(top_action,available_width*canvas.scale,[&](auto v){return renderer.text_width_pixels(v,pixels);});
        if(labels.size()>2){labels.resize(2);labels.back()=dialog_text::ellipsize(labels.back()+"…",available_width*canvas.scale,[&](auto v){return renderer.text_width_pixels(v,pixels);});}
        const float baseline=top_action_y*1080;
        for(std::size_t i=0;i<labels.size();++i)canvas.text(labels[i],x,baseline+i*pitch,28,{1,1,1,0.9F*open_alpha});
        top=std::max(top,baseline+(labels.size()-1)*pitch+metrics.y+24*ui);
    }
    const float bottom=1000,viewport=std::max(line_height,bottom-top);
    const auto window=dialog_text::choice_window(row_heights,selection_index,viewport,first_visible);
    first_visible=window.first;
    selected_page_lines=dialog_text::line_capacity(viewport,pitch,line_height);
    selected_total_lines=wrapped_choices.empty()?0:wrapped_choices[selection_index].size();
    text_scroll_line=std::min(text_scroll_line,selected_total_lines>selected_page_lines?selected_total_lines-selected_page_lines:0);
    renderer.set_clip((canvas.ox+1334*canvas.scale)/renderer.frame_size.width,
        (canvas.oy+top*canvas.scale)/renderer.frame_size.height,
        363*canvas.scale/renderer.frame_size.width,viewport*canvas.scale/renderer.frame_size.height);
    float y=top+metrics.x;
    for(std::size_t i=window.first;i<window.last;++i) {
        const double focus=1-std::clamp(std::abs(static_cast<double>(i)-selected),0.0,1.0);
        const float eased=static_cast<float>(focus*focus*(3-2*focus));
        const bool focused=i==selection_index;
        if(!swatches.empty()&&!focused&&i<swatches.size()) {
            canvas.rect(x,y-metrics.x,27,27,{swatches[i],open_alpha});
        } else {
            const std::size_t start=focused?text_scroll_line:0;
            const auto& lines=wrapped_choices[i];
            const auto count=std::min(lines.size()-start,selected_page_lines);
            for(std::size_t line=0;line<count;++line)
                canvas.text(lines[start+line],x,y+line*pitch,24+4*eased,{1,1,1,(0.84F+0.16F*eased)*open_alpha},focused?xmb_canvas::pulse():0);
        }
        y+=row_heights[i];
    }
    renderer.reset_clip();
    if(window.first)canvas.text("▲",x,top-12*ui,20,{1,1,1,open_alpha});
    if(window.last<choices.size())canvas.text("▼",x,1034,20,{1,1,1,open_alpha});
    if(selected_total_lines>selected_page_lines)canvas.text("← More text →",x,1064,16,{1,1,1,open_alpha});
}

}
