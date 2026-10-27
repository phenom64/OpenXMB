/* XMBShell, a console-like desktop shell
 * Copyright (C) 2025 - JCM
 *
 * This file (or substantial portions of it) is derived from XMBShell:
 *   https://github.com/JnCrMx/xmbshell
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
        float lift = (1.0f - settle) * 0.018f;
        renderer.push_color(glm::vec4(1.0f, 1.0f, 1.0f, settle));

        xmb_canvas c(renderer);
        c.rect(0,0,1920,1080,{0,0,0,0.25F});
        c.rect(0,199,1920,680,{0,0,0,0.12F});
        c.rect(0,199,1920,1,{1,1,1,0.75F});c.rect(0,879,1920,1,{1,1,1,0.75F});
        c.text(title,960,382,28,{1,1,1,1},0,true);
        std::vector<std::string> lines;
        std::string line;
        for(const auto part:std::views::split(message,' ')) {
            std::string word(part.begin(),part.end());
            const auto candidate=line.empty()?word:line+" "+word;
            if(!line.empty()&&renderer.text_width_pixels(candidate,24*c.scale)>1150*c.scale) {lines.push_back(line);line=word;}
            else line=candidate;
            if(line.find('\n')!=std::string::npos) {
                for(auto position=line.find('\n');position!=std::string::npos;position=line.find('\n')) {lines.push_back(line.substr(0,position));line.erase(0,position+1);}
            }
        }
        if(!line.empty()) lines.push_back(std::move(line));
        float baseline=500-(static_cast<int>(std::min(lines.size(),std::size_t{8}))-1)*17.0F;
        for(std::size_t i=0;i<std::min(lines.size(),std::size_t{8});++i) {c.text(lines[i],960,baseline,24,{1,1,1,1},0,true);baseline+=34;}
        float total=0;for(const auto& choice:choices) total+=renderer.text_width_pixels(choice,26*c.scale)/c.scale+120;
        float x=960-(total-120)*0.5F;
        for(std::size_t i=0;i<choices.size();++i) {
            const float width=renderer.text_width_pixels(choices[i],26*c.scale)/c.scale;
            c.text(choices[i],x+width*0.5F,650,26,{1,1,1,1},i==selected?xmb_canvas::pulse():0,true);x+=width+120;
        }
        xmb->render_controller_buttons(renderer,0.5F,0.84F,std::array{
            std::pair{action::ok,"Enter"},std::pair{cancelable?action::cancel:action::none,"Back"}});
        renderer.pop_color();
    }

    result message_overlay::on_action(action action) {
        switch(action) {
            case action::cancel:
                if(cancelable) {
                    if(cancel_callback) {
                        cancel_callback();
                    }
                    return result::success | result::close | result::cancel_sound;
                }
                return result::unsupported;
            case action::ok:
                if(confirm_callback) {
                    confirm_callback(selected);
                }
                return result::success | result::close | result::confirm_sound;
            case action::left:
                if(choices.empty()) {
                    return result::unsupported | result::error_rumble;
                }
                if(selected > 0) {
                    selected--;
                    return result::success | result::ok_sound;
                }
                return result::unsupported | result::error_rumble;
            case action::right:
                if(choices.empty()) {
                    return result::unsupported | result::error_rumble;
                }
                if(selected + 1 < choices.size()) {
                    selected++;
                    return result::success | result::ok_sound;
                }
                return result::unsupported | result::error_rumble;
            default:
                return result::unsupported;
        }
    }
}
