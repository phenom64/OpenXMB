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
#include "../localization/shell_locale.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <SDL2/SDL_scancode.h>
#include <SDL2/SDL_video.h>
#include <openxmb/video.hpp>
#include <openxmb/video_worker.hpp>
#include <openxmb/music/player.hpp>
#include "video_preferences.hpp"

export module openxmb.app:video_player;

import auroreengine;
import glm;
import spdlog;
import vulkan_hpp;
import vma;
import i18n;
import openxmb.utils;
import openxmb.config;
import openxmb.render;
import :component;
import :programs;
import :message_overlay;
import :base_viewer;
import :xmb_presentation;
import :main;

namespace programs {
using namespace app;
using namespace mfk::i18n::literals;
namespace video = openxmb::video;

export class video_player : private base_viewer, public component, public action_receiver,
                            public event_receiver, public joystick_receiver, public mouse_receiver {
public:
    float typography_scale() const override { return 2.5F; }

    video_player(std::filesystem::path file, auroreengine::resource_loader& loader)
        : device(loader.getDevice()), allocator(loader.getAllocator()) {
        for(const auto* icon:{"vp/scene_search","vp/screen_mode","vp/audio_options","vp/subtitle_options","system/mp_volume","system/mp_display","system/mp_prev","system/mp_next","vp/instant_replay","system/mp_play","vp/go_to","vp/frf_1_5","vp/frame_reverse","vp/frame_advance","system/mp_repeat","system/mp_addplaylist"}) icons_.load(loader,icon);
        image_width = image_height = 1;
        openxmb::music::service().set_external_duck(true);
        playback = std::make_unique<video::worker>();
        discover_playlist(file);
        open_path(std::move(file));
    }

    ~video_player() {
        remember_position(true);
        playback.reset();
        if(texture) { try { wait_gpu(); } catch(...) {} }
        texture.reset();
        openxmb::music::service().set_external_duck(false);
    }

    result tick(shell* xmb) override {
        window = xmb->get_window();
        auto snapshot = playback->snapshot();
        if(snapshot->loading) return result::success;
        if(!snapshot->ready || snapshot->status == video::state::failed) {
            xmb->emplace_overlay<app::message_overlay>("Failed to open video"_(), snapshot->error.empty() ? "Video could not be loaded"_() : snapshot->error, std::vector<std::string>{"OK"_()});
            return result::close;
        }
        if(!loaded) {
            loaded = true;
            apply_volume();
            playback->set_repeat(repetition == repeat_mode::file);
            if(config::CONFIG.enabled("video.resume")) {
                const double position=config::CONFIG.number(resume_key(),0);
                if(position>3&&position<snapshot->info.duration-3) playback->seek(position);
            }
            if(!snapshot->info.audio_tracks.empty() && snapshot->info.audio_status != "Audio output active") status_message = snapshot->info.audio_status;
        }
        apply_volume();
        apply_preferences();
        const bool focused=window->config.headless||(SDL_GetWindowFlags(window->win.get())&SDL_WINDOW_INPUT_FOCUS)!=0;
        const auto focus=focus_pause.update(focused,config::CONFIG.enabled("audio.background"),snapshot->status);
        if(focus==video_preferences::focus_action::pause) playback->pause(true);
        else if(focus==video_preferences::focus_action::resume) playback->pause(false);
        if(std::chrono::steady_clock::now()-last_resume_save>std::chrono::seconds(10)) remember_position(true);
        if(!snapshot->error.empty()) status_message = snapshot->error;
        if(auto frame = playback->take_frame()) pending_frame = std::move(frame);
        if(playback->playback_state() == video::state::failed) {
            xmb->emplace_overlay<app::message_overlay>("Video playback failed"_(), playback->error(), std::vector<std::string>{"OK"_()});
            return result::close;
        }
        if(playback->playback_state() != video::state::ended) end_handled = false;
        if(playback->playback_state() == video::state::ended && !end_handled) {
            end_handled = true;
            if(playlist_index + 1 < playlist.size() || repetition == repeat_mode::all) advance_file(1);
        }
        if(screen == screen_mode::zoom) base_viewer::tick();
        return result::success;
    }

    void prerender(vk::CommandBuffer cmd, int frame_index, shell* xmb) override {
        window = xmb->get_window();
        if(!pending_frame) return;
        auto& frame = *pending_frame;
        try {
            if(!texture) texture=std::make_unique<render::video_texture>(device,allocator,
                render::video_texture::supports_native(window->physicalDevice,window->queueFamilyIndices.graphicsFamily.value()),
                [this] {return render::shaders::yuv420p::decode_comp(device);},[this] {wait_gpu();});
            if(texture->upload(cmd,frame_index,window->swapchainImageCount,frame)) {
                image_initialized=true;
                image_height=frame.height;
                image_width=static_cast<unsigned int>(std::max(1.0,frame.aspect_ratio*frame.height));
            }
        } catch(const std::exception& error) {status_message=std::string("Video upload failed: ")+error.what();}
        pending_frame.reset();
    }

    void render(auroreengine::gui_renderer& renderer, shell* xmb) override {
        renderer.draw_rect({0, 0}, {1, 1}, {0, 0, 0, 1});
        if(!loaded) {
            renderer.draw_text("Opening video…"_(), 0.5f, 0.5f, 0.025f, glm::vec4(1), true, true);
            return;
        }
        if(texture && texture->ready() && image_initialized) {
            if(screen == screen_mode::stretch) renderer.draw_image(texture->view(), 0, 0, renderer.aspect_ratio, 1);
            else if(screen==screen_mode::fill) {
                const float aspect=static_cast<float>(playback->info().aspect_ratio);
                const float height=std::max(1.0F,static_cast<float>(renderer.aspect_ratio)/std::max(0.01F,aspect));
                const float width=height*aspect;
                renderer.draw_image(texture->view(),(1-width/renderer.aspect_ratio)/2,(1-height)/2,width,height);
            }
            else if(screen == screen_mode::original) {
                float h = std::min(1.0f, static_cast<float>(texture->height()) / std::max(1u, xmb->get_window()->swapchainExtent.height));
                float w = h * static_cast<float>(playback->info().aspect_ratio);
                if(w > renderer.aspect_ratio) { h *= renderer.aspect_ratio / w; w = renderer.aspect_ratio; }
                renderer.draw_image(texture->view(), (1 - w / renderer.aspect_ratio) / 2, (1 - h) / 2, w, h);
            } else {
                float saved_zoom = zoom;
                if(screen == screen_mode::fit) zoom = 1;
                base_viewer::render(texture->view(), 1.0f, renderer);
                zoom = saved_zoom;
            }
        }
        draw_subtitles(renderer);
        bool controls = menu != menu_kind::none || playback->playback_state() != video::state::playing ||
            std::chrono::steady_clock::now() - last_input < std::chrono::seconds(4);
        if(controls) {
            app::xmb_canvas c(renderer);
            c.rect(0,0,1920,130,{0,0,0,0.64F});c.text(path.filename().string(),132,107,27);
            const float progress=playback->info().duration>0?static_cast<float>(std::clamp(playback->position()/playback->info().duration,0.0,1.0)):0;
            c.rect(0,920,1920,160,{0,0,0,0.48F});
            icons_.draw(c,playback->playback_state()==video::state::playing?"system/mp_play":"system/mp_display",130,971,58,1);
            if(repetition!=repeat_mode::off) icons_.draw(c,"system/mp_repeat",240,971,58,1);
            c.text(time_label(playback->position()),850,978,25);
            c.rect(1000,962,560,12,{0.38F,0.38F,0.38F,1});c.rect(1001,963,558,10,{0.04F,0.04F,0.04F,1});
            c.rect(1001,963,558*progress,10,{0.32F,0.51F,0.02F,1});c.rect(1001,963,558*progress,3,{0.78F,0.90F,0.30F,1});
            c.text(time_label(playback->info().duration),1625,978,25);
            if(!status_message.empty()) c.text(status_message,100,885,24);

        }
        if(menu != menu_kind::none) draw_menu(renderer);
        if(show_information) {
            const auto& info = playback->info();
            renderer.draw_rect({0.03f, 0.05f}, {0.49f, 0.18f}, {0, 0, 0, 0.8f});
            renderer.draw_text(std::to_string(info.width) + " × " + std::to_string(info.height) + " · " + utils::to_fixed_string(info.frame_rate, 2) + " fps", 0.05f, 0.09f, 0.018f);
            renderer.draw_text(info.decoder, 0.05f, 0.13f, 0.016f);
            renderer.draw_text(info.audio_status, 0.05f, 0.17f, 0.016f);
            if(texture) {
                const auto& stats=texture->stats();
                const auto pipeline=std::string(texture->gpu_conversion()?"Native YUV GPU conversion":"RGBA upload")+" · "+std::to_string(stats.uploaded_bytes)+" upload bytes";
                renderer.draw_text(pipeline,0.05F,0.25F,0.014F);
            }
            if(playback->rate() != 1.0) renderer.draw_text("Audio is muted during speed playback"_(), 0.05f, 0.21f, 0.014f);
        }
    }

    result on_action(action command) override {
        last_input = std::chrono::steady_clock::now();
        if(menu != menu_kind::none) return menu_action(command);
        if(command == action::cancel) return result::close;
        if(!loaded) return result::success;
        switch(command) {
            case action::ok: focus_pause.cancel();playback->pause(playback->playback_state() == video::state::playing); break;
            case action::left: playback->seek(playback->position() - 10); break;
            case action::right: playback->seek(playback->position() + 10); break;
            case action::up: set_volume(volume + 0.05f); break;
            case action::down: set_volume(volume - 0.05f); break;
            case action::extra: playback->replay(); break;
            case action::options: open_menu(menu_kind::main); break;
            default: return result::unsupported;
        }
        return result::success;
    }

    result on_event(const event& event) override {
        if(event.action!=action::none) return on_action(event.action);
        if(auto* key = event.get<events::key_down>(); key && loaded) {
            switch(key->keycode) {
                case SDL_SCANCODE_SPACE: return on_action(action::ok);
                case SDL_SCANCODE_N: advance_file(1); return result::success;
                case SDL_SCANCODE_P: advance_file(-1); return result::success;
                case SDL_SCANCODE_PERIOD: playback->step(1); return result::success;
                case SDL_SCANCODE_COMMA: playback->step(-1); return result::success;
                case SDL_SCANCODE_F: screen = static_cast<screen_mode>((static_cast<int>(screen) + 1) % 5); return result::success;
                case SDL_SCANCODE_S: open_menu(menu_kind::subtitles); return result::success;
                case SDL_SCANCODE_A: open_menu(menu_kind::audio); return result::success;
            }
        }
        if(auto* button = event.get<events::controller_button_down>(); button && loaded) {
            if(button->button == events::logical_controller_button::leftshoulder) { advance_file(-1); return result::success; }
            if(button->button == events::logical_controller_button::rightshoulder) { advance_file(1); return result::success; }
            if(button->button == events::logical_controller_button::start) return on_action(action::ok);
        }
        if(auto* axis = event.get<events::joystick_axis>()) return on_joystick(static_cast<unsigned int>(axis->index), axis->x, axis->y);
        if(auto* move = event.get<events::mouse_move>()) return on_mouse_move(move->x, move->y);
        if(auto* wheel = event.get<events::mouse_scroll>()) return on_mouse_scroll(wheel->x);
        if(auto* button = event.get<events::mouse_button_down>(); button && button->button == events::logical_mouse_button::left) return on_action(action::ok);
        return on_action(event.action);
    }

    result on_joystick(unsigned int index, float x, float y) override {
        if(screen == screen_mode::zoom && index == 0) return base_viewer::on_joystick(index, x, y);
        return result::unsupported;
    }
    result on_mouse_move(float x, float y) override {
        last_input = std::chrono::steady_clock::now();
        return screen == screen_mode::zoom ? base_viewer::on_mouse_move(x, y) : result::success;
    }
    result on_mouse_scroll(float amount) override { set_volume(volume + amount * 0.05f); return result::success; }
    bool is_opaque() const override { return true; }
    bool keeps_awake() const override {return playback&&loaded&&playback->playback_state()==video::state::playing;}

private:
    app::media_icons icons_;
    enum class repeat_mode { off, file, all };
    enum class screen_mode { fit, original, zoom, stretch, fill };
    enum class menu_kind { none, main, audio, subtitles, screen, speed, scenes, files };
    vk::Device device;
    vma::Allocator allocator;
    std::filesystem::path path;
    std::vector<std::filesystem::path> playlist;
    std::size_t playlist_index = 0;
    auroreengine::window* window = nullptr;
    bool loaded = false;
    std::unique_ptr<video::worker> playback;
    std::optional<video::frame> pending_frame;
    std::unique_ptr<render::video_texture> texture;
    bool image_initialized = false;
    bool end_handled = false;
    float volume = 1;
    float applied_volume=-1;
    video_preferences::focus_policy focus_pause;
    std::string applied_audio_language,applied_subtitle_language,applied_fit;
    std::chrono::steady_clock::time_point last_resume_save=std::chrono::steady_clock::now();
    repeat_mode repetition = repeat_mode::off;
    screen_mode screen = screen_mode::fit;
    menu_kind menu = menu_kind::none;
    std::size_t selection = 0;
    bool show_information = false;
    double scene_return = 0;
    bool scene_was_paused = false;
    std::string status_message;
    std::chrono::steady_clock::time_point last_input = std::chrono::steady_clock::now();

    static std::string time_label(double value) {
        int seconds = static_cast<int>(std::max(0.0, value));
        std::string result = std::to_string(seconds / 60) + ":";
        if(seconds % 60 < 10) result += '0';
        return result + std::to_string(seconds % 60);
    }

    void discover_playlist(const std::filesystem::path& current) {
        std::tie(playlist,playlist_index)=video_preferences::discover_playlist(current);
    }

    void open_path(std::filesystem::path next) {
        remember_position(true);
        loaded = false;
        status_message.clear();
        if(texture) wait_gpu();
        texture.reset();
        path = std::move(next);
        pending_frame.reset();
        image_initialized = false;
        end_handled = false;
        menu = menu_kind::none;
        applied_volume=-1;applied_audio_language.clear();applied_subtitle_language.clear();applied_fit.clear();
        focus_pause.cancel();
        video::options settings;
        settings.hardware_acceleration = config::CONFIG.preference("video.hardware",OPENXMB_HWACCEL_STR);
        settings.native_yuv=true;
        playback->set_path(path, std::move(settings));
        last_input = std::chrono::steady_clock::now();
    }

    void advance_file(int direction) {
        if(!loaded || playlist.empty()) return;
        if(direction < 0) playlist_index = playlist_index == 0 ? playlist.size() - 1 : playlist_index - 1;
        else playlist_index = (playlist_index + 1) % playlist.size();
        open_path(playlist[playlist_index]);
    }
    void wait_gpu() {
        if(window) window->waitIdle();
        else device.waitIdle();
    }

    void set_volume(float value) {
        volume = std::clamp(value, 0.0f, 1.0f);
        apply_volume();
        last_input = std::chrono::steady_clock::now();
    }
    void apply_volume() {
        const float gain=video_preferences::output_gain(volume,config::CONFIG.number("audio.master",100),config::CONFIG.number("audio.video",100));
        if(playback&&std::abs(gain-applied_volume)>0.0001F) {playback->set_volume(gain);applied_volume=gain;}
    }
    void apply_preferences() {
        const auto audio=config::CONFIG.preference("video.audio.language","Default");
        if(audio!=applied_audio_language) {
            if(auto track=video_preferences::language_track(playback->info().audio_tracks,audio)) playback->select_audio(*track);
            applied_audio_language=audio;
        }
        const auto subtitle=config::CONFIG.preference("video.subtitle.language","Off");
        if(subtitle!=applied_subtitle_language) {
            const auto tracks=playback->info().subtitle_tracks;
            if(subtitle=="Off") playback->select_subtitle(-1);
            else if(subtitle=="Default"&&!tracks.empty()) playback->select_subtitle(tracks.front().index);
            else if(auto track=video_preferences::language_track(tracks,subtitle)) playback->select_subtitle(*track);
            applied_subtitle_language=subtitle;
        }
        const auto fit=config::CONFIG.preference("video.fit","Fit");
        if(fit!=applied_fit) {screen=fit=="Fill"?screen_mode::fill:fit=="Original"?screen_mode::original:screen_mode::fit;applied_fit=fit;}
    }
    std::string resume_key() const {const auto encoded=path.lexically_normal().generic_u8string();return "video.position."+std::string(encoded.begin(),encoded.end());}
    void remember_position(bool save) {
        if(!loaded||!playback||!config::CONFIG.enabled("video.resume")||path.empty()) return;
        const auto position=playback->position(),duration=playback->info().duration;
        if(position>3&&position<duration-3) config::CONFIG.preferences[resume_key()]=std::to_string(position);
        else config::CONFIG.preferences.erase(resume_key());
        if(save) {try {config::CONFIG.save_config();}catch(const std::exception& error) {status_message=std::string("Cannot save video position: ")+error.what();}}
        last_resume_save=std::chrono::steady_clock::now();
    }

    void draw_subtitles(auroreengine::gui_renderer& renderer) {
        auto text = playback->subtitle_text();
        if(text.empty()) return;
        const auto scale=static_cast<float>(std::clamp(config::CONFIG.number("video.subtitle.size",1),0.5,2.0));
        const auto columns=static_cast<std::size_t>(75/scale);
        std::vector<std::string> lines;
        std::size_t offset = 0;
        while(offset < text.size() && lines.size() < 5) {
            auto end = text.find('\n', offset);
            auto line = text.substr(offset, end == std::string::npos ? end : end - offset);
            // Bound long dialogue to the available screen width without dropping
            // trailing text. FFmpeg has already stripped ASS/HTML formatting.
            while(line.size() > columns && lines.size() < 5) {
                auto split = line.rfind(' ', columns);
                if(split == std::string::npos || split == 0) split = columns;
                lines.push_back(line.substr(0, split));
                line.erase(0, split + (line[split] == ' '));
            }
            if(lines.size() < 5) lines.push_back(std::move(line));
            if(end == std::string::npos) break;
            offset = end + 1;
        }
        float y = 0.85f - static_cast<float>(lines.size()) * 0.035f*scale;
        for(const auto& line : lines) {
            renderer.draw_text(line, 0.502f, y + 0.002f, 0.025f*scale, {0, 0, 0, 1}, true, true);
            renderer.draw_text(line, 0.5f, y, 0.025f*scale, glm::vec4(1), true, true);
            y += 0.035f*scale;
        }
    }

    void open_menu(menu_kind kind) {
        menu = kind;
        selection = 0;
        last_input = std::chrono::steady_clock::now();
        if(kind == menu_kind::scenes) {
            scene_return = playback->position();
            scene_was_paused = playback->playback_state() != video::state::playing;
            focus_pause.cancel();
            playback->pause(true);
            preview_scene();
        }
    }
    void preview_scene() { playback->seek(playback->info().duration * static_cast<double>(selection) / 12.0); }

    std::vector<std::string> menu_rows() const {
        if(!playback) return {};
        switch(menu) {
            case menu_kind::main: return {
                playback->playback_state() == video::state::playing ? "Pause"_() : "Play"_(),
                "Replay 10 seconds"_(), "Play from beginning"_(), "Previous video"_(), "Next video"_(),
                ::openxmb::localization::shell_locale::translate("Repeat")+": "+::openxmb::localization::shell_locale::translate(repetition == repeat_mode::off ? "Off" : repetition == repeat_mode::file ? "This video" : "All videos"),
                "Screen mode"_(), "Audio track"_(), "Subtitles"_(), "Playback speed"_(), "Scene search"_(),
                "Previous frame"_(), "Next frame"_(), "Volume −"_(), "Volume +"_(), "Video information"_(), "Video list"_()
            };
            case menu_kind::screen: return {"Normal (fit)"_(), "Original size"_(), "Zoom"_(), "Full screen (stretch)"_(), "Zoom in"_(), "Zoom out"_()};
            case menu_kind::speed: return {"¼ speed (muted)"_(), "½ speed (muted)"_(), "Normal speed"_(), "1½ speed (muted)"_(), "Double speed (muted)"_()};
            case menu_kind::audio: {
                std::vector<std::string> rows;
                for(const auto& track : playback->info().audio_tracks) rows.push_back((track.index == playback->selected_audio() ? "✓ " : "") + track.label);
                if(rows.empty()) rows.push_back("No audio tracks"_());
                return rows;
            }
            case menu_kind::subtitles: {
                std::vector<std::string> rows{playback->selected_subtitle() == -1 ? "✓ Off"_() : "Off"_()};
                for(const auto& track : playback->info().subtitle_tracks) rows.push_back((track.index == playback->selected_subtitle() ? "✓ " : "") + track.label);
                return rows;
            }
            case menu_kind::scenes: {
                std::vector<std::string> rows;
                for(int i = 0; i < 12; ++i) rows.push_back(time_label(playback->info().duration * i / 12.0));
                return rows;
            }
            case menu_kind::files: {
                std::vector<std::string> rows;
                for(std::size_t i = 0; i < playlist.size(); ++i) rows.push_back((i == playlist_index ? "✓ " : "") + playlist[i].filename().string());
                return rows;
            }
            default: return {};
        }
    }

    void draw_menu(auroreengine::gui_renderer& renderer) {
        auto rows = menu_rows();
        app::xmb_canvas c(renderer);
        if(menu==menu_kind::main) {
            static constexpr std::array<int,17> actions{10,6,7,8,13,14,15,3,4,1,0,2,9,11,12,5,16};
            static constexpr std::array<std::string_view,17> icons{"vp/scene_search","vp/screen_mode","vp/audio_options","vp/subtitle_options","system/mp_volume","system/mp_volume","system/mp_display","system/mp_prev","system/mp_next","vp/instant_replay","system/mp_play","vp/go_to","vp/frf_1_5","vp/frame_reverse","vp/frame_advance","system/mp_repeat","system/mp_addplaylist"};
            for(int row=0;row<3;++row) {const int count=row==0?7:row==1?8:2,first=row==0?0:row==1?7:15;
                for(int column=0;column<count;++column) {const int i=first+column;const bool focused=selection==actions[i];
                    icons_.draw(c,icons[i],562+(column-(count-1)*0.5F)*64,412+row*65,74,focused?1:0.85F,focused?1.3F:1);
                }
            }
            if(selection<rows.size()) c.text(rows[selection],562,640,26,{1,1,1,1},app::xmb_canvas::pulse(),true);
            return;
        }
        c.rect(1100,0,770,1080,{0,0,0,0.6F});
        const auto title=menu==menu_kind::scenes?"Scene Search":menu==menu_kind::subtitles?"Subtitle Options":menu==menu_kind::audio?"Audio Options":"Video Settings";
        c.text(::openxmb::localization::shell_locale::translate(title),1160,95,28);
        const auto first=selection>8?selection-8:0;
        for(std::size_t i=first;i<rows.size()&&i<first+16;++i)
            c.text(rows[i],1160,155+static_cast<float>(i-first)*50,26,{1,1,1,selection==i?1.0F:0.65F},selection==i?app::xmb_canvas::pulse():0);
    }

    result menu_action(action command) {
        auto rows = menu_rows();
        if(rows.empty()) { menu = menu_kind::none; return result::success; }
        if(command == action::cancel || command == action::options) {
            if(menu == menu_kind::scenes) { playback->seek(scene_return); playback->pause(scene_was_paused); }
            menu = menu == menu_kind::main ? menu_kind::none : menu_kind::main;
            selection = 0;
            return result::success;
        }
        if(menu==menu_kind::main&&(command==action::left||command==action::right||command==action::up||command==action::down)) {
            static constexpr std::array<int,17> actions{10,6,7,8,13,14,15,3,4,1,0,2,9,11,12,5,16};
            const auto found=std::find(actions.begin(),actions.end(),selection);const int i=found==actions.end()?0:static_cast<int>(found-actions.begin());
            int row=i<7?0:i<15?1:2,column=i-(row==0?0:row==1?7:15);const std::array<int,3> count{7,8,2};
            if(command==action::left||command==action::right) column=(column+count[row]+(command==action::left?-1:1))%count[row];
            else {row=(row+(command==action::up?2:1))%3;column=std::min(column,count[row]-1);}
            selection=actions[(row==0?0:row==1?7:15)+column];return result::success;
        }
        if(command == action::up || command == action::down) {
            selection = command == action::up ? (selection + rows.size() - 1) % rows.size() : (selection + 1) % rows.size();
            if(menu == menu_kind::scenes) preview_scene();
            return result::success;
        }
        if(command != action::ok) return result::success;
        if(menu == menu_kind::main) {
            switch(selection) {
                case 0: focus_pause.cancel();playback->pause(playback->playback_state() == video::state::playing); menu = menu_kind::none; break;
                case 1: playback->replay(); menu = menu_kind::none; break;
                case 2: focus_pause.cancel();playback->seek(0); playback->pause(false); menu = menu_kind::none; break;
                case 3: advance_file(-1); break;
                case 4: advance_file(1); break;
                case 5: repetition = static_cast<repeat_mode>((static_cast<int>(repetition) + 1) % 3); playback->set_repeat(repetition == repeat_mode::file); break;
                case 6: open_menu(menu_kind::screen); break;
                case 7: open_menu(menu_kind::audio); break;
                case 8: open_menu(menu_kind::subtitles); break;
                case 9: open_menu(menu_kind::speed); break;
                case 10: if(playback->info().duration > 0) open_menu(menu_kind::scenes); break;
                case 11: playback->step(-1); menu = menu_kind::none; break;
                case 12: playback->step(1); menu = menu_kind::none; break;
                case 13: set_volume(volume - 0.05f); break;
                case 14: set_volume(volume + 0.05f); break;
                case 15: show_information = !show_information; menu = menu_kind::none; break;
                case 16: open_menu(menu_kind::files); selection = playlist_index; break;
            }
        } else {
            if(menu == menu_kind::screen) {
                if(selection < 4) screen = static_cast<screen_mode>(selection);
                else { screen = screen_mode::zoom; zoom = std::clamp(zoom * (selection == 4 ? 1.2f : 1.0f / 1.2f), 0.25f, 8.0f); }
            } else if(menu == menu_kind::speed) {
                constexpr std::array<double, 5> rates{0.25, 0.5, 1.0, 1.5, 2.0};
                playback->set_rate(rates[selection]);
            } else if(menu == menu_kind::audio) {
                if(selection < playback->info().audio_tracks.size()) playback->select_audio(playback->info().audio_tracks[selection].index);
            } else if(menu == menu_kind::subtitles) {
                playback->select_subtitle(selection == 0 ? -1 : playback->info().subtitle_tracks[selection - 1].index);
            } else if(menu == menu_kind::scenes) {
                preview_scene();
                playback->pause(false);
            } else if(menu == menu_kind::files) {
                playlist_index = selection;
                open_path(playlist[playlist_index]);
            }
            menu = menu_kind::none;
        }
        return result::success;
    }
};

namespace {
#if OPENXMB_ENABLE_VIDEO_PLAYER
const inline register_program<video_player> video_player_program{
    "video_player",
    {"video/mp4", "video/x-matroska", "video/x-msvideo", "video/quicktime", "video/webm", "video/mpeg", "video/ogg", "video/mp2t"},
    {".mp4", ".mkv", ".avi", ".mov", ".webm", ".mpg", ".mpeg", ".m4v", ".ts", ".ogv"}
};
#endif
}
}
