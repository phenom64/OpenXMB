/* XMBShell, a console-like desktop shell
 * Copyright (C) 2025 - JCM
 *
 * This file (or substantial portions of it) is derived from XMBShell:
 *   https://github.com/JnCrMx/xmbshell
 *
 * Modified by Syndromatic Ltd for OpenXMB.
 * Portions Copyright (C) 2025 Syndromatic Ltd, Kavish Krishnakumar.
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
#include "input_timing.hpp"
#include "../localization/shell_locale.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>
#include <map>
namespace openxmb::themes { class ThemeLibrary; struct AppliedTheme; }

#include <glm/vec2.hpp>
#include "openxmb/xmb/boot_timeline.hpp"

export module openxmb.app:main;

import openxmb.render;
import openxmb.utils;
import openxmb.xmb.captured_wave_renderer;
import openxmb.xmb.monthly_background_renderer;
import openxmb.xmb.status_bar_renderer;
import auroreengine;
import glm;
import sdl2;
import vulkan_hpp;

import :component;
import :choice_overlay;
import :main_menu;
import :message_overlay;
import :news_display;
import :progress_overlay;
import :startup_overlay;

namespace app
{
    export using clipboard = std::variant<
        std::string, std::function<std::string()>,
        std::function<bool(std::filesystem::path)>
    >;

    enum class transition_direction {
        in, out
    };

    using namespace auroreengine;
    export class shell : public phase, public input::keyboard_handler, public input::controller_handler
    {
        public:
            shell(window* window);
            ~shell();
            void preload() override;
            void prepare(std::vector<vk::Image> swapchainImages, std::vector<vk::ImageView> swapchainViews) override;
            void render(int frame, vk::Semaphore imageAvailable, vk::Semaphore renderFinished, vk::Fence fence) override;
            void tick();
            openxmb::xmb::BootSample startup_sample();
            void dismiss_startup_warning(const event& input);

            void key_up(sdl::Keysym key) override;
            void key_down(sdl::Keysym key) override;

            void add_controller(sdl::GameController* controller) override;
            void remove_controller(sdl::GameController* controller) override;
            void button_down(sdl::GameController* controller, sdl::GameControllerButton button) override;
            void button_up(sdl::GameController* controller, sdl::GameControllerButton button) override;
            void axis_motion(sdl::GameController* controller, sdl::GameControllerAxis axis, int16_t value) override;

            void reload_background();
            void reload_fonts();
            void set_font_file(const std::string& path);
            void reset_settings();
            void refresh_desktop_preferences();
            void reload_button_icons();
            void reload_theme_sounds();
            void reload_language();
            void reload_menu();
            void open_panel(const std::string& name);
            bool set_audio_device(const std::string& device);
            std::string audio_device_name() const { return active_audio_device; }
            void open_media(const std::filesystem::path& path);
            void switch_profile(const std::filesystem::path& path);
            void apply_theme(const openxmb::themes::AppliedTheme& theme);
            std::shared_ptr<openxmb::themes::ThemeLibrary> theme_library();
            void queue_panel(std::string name) { pending_panel = std::move(name); }
            void queue_media(std::filesystem::path path) { pending_media = std::move(path); }

            void dispatch(const event& event);
            template<typename T, typename... Args>
            void dispatch(action action, Args&&... args) {
                dispatch(event{action, T{std::forward<Args>(args)...}});
            }
            void dispatch(action action) {
                dispatch(event{action, std::monostate{}});
            }
            void handle(result result);

            std::string get_controller_type() const;
            const texture* get_button_icon(action value) const { return buttonTextures[std::to_underlying(value)].get(); }
            std::string get_button_label(action value) const { return button_labels[std::to_underlying(value)]; }
            void render_controller_buttons(gui_renderer& renderer, float x, float y, std::ranges::range auto buttons) {
                const float fw=static_cast<float>(renderer.frame_size.width),fh=static_cast<float>(renderer.frame_size.height);
                const float scale=std::min(fw/1920.0F,fh/1080.0F),height=34*scale,gap=12*scale,spacing=64*scale;
                const float font=28*scale*static_cast<float>(config::CONFIG.textScale),key_font=19*scale;
                const auto glyph_width=[&](action a){const auto& label=button_labels[std::to_underlying(a)];return label.empty()?height:std::max(height,renderer.text_width_pixels(label,key_font)+14*scale);};
                float total=0;for(const auto& [a,text]:buttons)if(a!=action::none)total+=glyph_width(a)+gap+renderer.text_width_pixels(::openxmb::localization::shell_locale::translate(text),font)+spacing;
                if(total>0)total-=spacing;float current=x*fw-total*0.5F;
                const auto metrics=renderer.text_line_metrics_pixels(font),key_metrics=renderer.text_line_metrics_pixels(key_font);
                for(const auto& [a,text]:buttons) {
                    if(a==action::none)continue;const auto i=std::to_underlying(a);const auto width=glyph_width(a);
                    if(!button_labels[i].empty()) {
                        renderer.draw_rect({current/fw,y-height/(2*fh)},{width/fw,height/fh},{1,1,1,0.75F});
                        renderer.draw_rect({(current+scale)/fw,y-height/(2*fh)+scale/fh},{(width-2*scale)/fw,(height-2*scale)/fh},{0.08F,0.08F,0.08F,1});
                        renderer.draw_text_pixels(button_labels[i],current+width*0.5F,y*fh+(key_metrics.x-key_metrics.y)*0.5F,key_font,{1,1,1,1},0.7F,0,true);
                    } else if(auto* icon=buttonTextures[i].get())renderer.draw_image(*icon,current/fw,y-height/(2*fh),height/fh,height/fh);
                    renderer.draw_text_pixels(::openxmb::localization::shell_locale::translate(text),current+width+gap,y*fh+(metrics.x-metrics.y)*0.5F,font,{1,1,1,1},0.7F,0,false);
                    current+=width+gap+renderer.text_width_pixels(::openxmb::localization::shell_locale::translate(text),font)+spacing;
                }
            }

            // Message-screen hints use fixed slots rather than packed labels: in
            // the 1920x1080 captures the Cross glyph sat at x~735 and the Circle
            // glyph at x~1165 whatever the label ("Edit", "Enter"), at y~906.
            void render_message_footer(gui_renderer& renderer, std::ranges::range auto buttons, float alpha) {
                const float fw=static_cast<float>(renderer.frame_size.width),fh=static_cast<float>(renderer.frame_size.height);
                const float scale=std::min(fw/1920.0F,fh/1080.0F),ox=(fw-1920.0F*scale)*0.5F,oy=(fh-1080.0F*scale)*0.5F;
                const float icon=22*scale,font=22*scale*static_cast<float>(config::CONFIG.textScale),key_font=15*scale;
                std::size_t count=0;for(const auto& [a,text]:buttons)if(a!=action::none)++count;
                constexpr std::array<float,2> pair_slots{735.0F,1165.0F};
                const auto metrics=renderer.text_line_metrics_pixels(font),key_metrics=renderer.text_line_metrics_pixels(key_font);
                const float centre_y=oy+906*scale;
                std::size_t slot=0;
                for(const auto& [a,text]:buttons) {
                    if(a==action::none)continue;
                    const float cx=ox+(count==2?pair_slots[slot]:count==1?900.0F:620.0F+slot*300.0F)*scale;
                    ++slot;
                    const auto i=std::to_underlying(a);
                    float label_x=cx+icon*0.5F+19*scale;
                    if(!button_labels[i].empty()) {
                        const float width=std::max(icon,renderer.text_width_pixels(button_labels[i],key_font)+12*scale);
                        renderer.draw_rect({(cx-icon*0.5F)/fw,(centre_y-icon*0.5F)/fh},{width/fw,icon/fh},{1,1,1,0.75F*alpha});
                        renderer.draw_rect({(cx-icon*0.5F+scale)/fw,(centre_y-icon*0.5F+scale)/fh},{(width-2*scale)/fw,(icon-2*scale)/fh},{0.08F,0.08F,0.08F,alpha});
                        renderer.draw_text_pixels(button_labels[i],cx-icon*0.5F+width*0.5F,centre_y+(key_metrics.x-key_metrics.y)*0.5F,key_font,{1,1,1,alpha},0.7F,0,true);
                        label_x=cx-icon*0.5F+width+19*scale;
                    } else if(auto* texture=buttonTextures[i].get()) {
                        renderer.draw_image(*texture,(cx-icon*0.5F)/fw,(centre_y-icon*0.5F)/fh,icon/fh,icon/fh,{1,1,1,alpha});
                    }
                    renderer.draw_text_pixels(::openxmb::localization::shell_locale::translate(text),label_x,centre_y+(metrics.x-metrics.y)*0.5F,font,{1,1,1,alpha},0.7F,0,false);
                }
            }

            auroreengine::window* get_window() const { return this->win; }

            void set_ingame_mode(bool ingame_mode) { this->ingame_mode = ingame_mode; }
            bool get_ingame_mode() const { return ingame_mode; }

            void set_background_only(bool background_only) {
                this->background_only = background_only;
                if(!this->background_only && !fixed_components_loaded) {
                    preload_fixed_components();
                }
            }
            bool get_background_only() const { return background_only; }

            void set_blur_background(bool blur) {
                if (blur == blur_background) return;
                blur_background = blur;
                last_blur_background_change = std::chrono::steady_clock::now();
            }
            bool get_blur_background() const { return blur_background; }
            void set_startup_background_blur_px(float blur_px) {
                startup_background_blur_px = std::max(0.0f, blur_px);
            }
            bool has_choice_overlay() const {
                for(const auto& overlay : overlays) {
                    if(dynamic_cast<app::choice_overlay*>(overlay.get()) != nullptr) {
                        return true;
                    }
                }
                return old_overlay &&
                    dynamic_cast<app::choice_overlay*>(old_overlay.get()) != nullptr;
            }

            app::component* push_overlay(std::unique_ptr<app::component>&& component) {
                auto ptr = component.get();
                overlays.emplace_back(std::move(component));
                overlay_transition_duration = ptr->fade_duration();
                overlay_fade_zoom = ptr->fade_zoom();

                if(ptr->do_fade_in()) {
                    overlay_fade_direction = transition_direction::in;
                    overlay_fade_time = std::chrono::steady_clock::now();
                } else {
                    overlay_fade_time = std::chrono::steady_clock::now() - overlay_transition_duration;
                }

                // Auto-enable background blur for modal message overlays + play question sound
                if(dynamic_cast<app::message_overlay*>(ptr) != nullptr) {
                    set_blur_background(true);
                    if(question_sound && sdl::mix::PlayChannel(-1, question_sound.get(), 0) == -1) {
                        spdlog::debug("PlayChannel(question): {}", sdl::mix::GetError());
                    }
                }
                return ptr;
            }
            template<typename T, typename... Args>
            T* emplace_overlay(Args&&... args) {
                auto p = std::make_unique<T>(std::forward<Args>(args)...);
                auto ptr = p.get();
                overlays.emplace_back(std::move(p));
                overlay_transition_duration = ptr->fade_duration();
                overlay_fade_zoom = ptr->fade_zoom();

                if(ptr->do_fade_in()) {
                    overlay_fade_direction = transition_direction::in;
                    overlay_fade_time = std::chrono::steady_clock::now();
                } else {
                    overlay_fade_time = std::chrono::steady_clock::now() - overlay_transition_duration;
                }

                if(dynamic_cast<app::message_overlay*>(ptr) != nullptr) {
                    set_blur_background(true);
                    if(question_sound && sdl::mix::PlayChannel(-1, question_sound.get(), 0) == -1) {
                        spdlog::debug("PlayChannel(question): {}", sdl::mix::GetError());
                    }
                }
                return ptr;
            }
            void remove_overlay(unsigned int index) {
                if(index >= overlays.size()) return;
                overlay_transition_duration = overlays[index]->fade_duration();
                overlay_fade_zoom = overlays[index]->fade_zoom();
                if(index == overlays.size()-1 && overlays[index]->do_fade_out()) {
                    overlay_fade_direction = transition_direction::out;
                    overlay_fade_time = std::chrono::steady_clock::now();
                    if(old_overlay) win->waitIdle();
                    old_overlay = std::move(overlays[index]);
                } else {
                    overlay_fade_time = std::chrono::steady_clock::now() - overlay_transition_duration;
                    win->waitIdle();
                }
                overlays.erase(overlays.begin()+index);

                // Disable blur when no more message overlays are present (including fading old_overlay)
                auto has_msg = false;
                for(auto& ov : overlays) {
                    if(dynamic_cast<app::message_overlay*>(ov.get()) != nullptr) { has_msg = true; break; }
                }
                if(!has_msg) {
                    if(old_overlay && dynamic_cast<app::message_overlay*>(old_overlay.get()) != nullptr && overlay_fade_direction == transition_direction::out) {
                        // keep blur until fade-out completes
                    } else {
                        set_blur_background(false);
                    }
                }
            }

            void set_clipboard(clipboard&& clipboard) {
                this->clipboard = std::move(clipboard);
            }
            const std::optional<clipboard>& get_clipboard() const { return clipboard; }
        private:
            friend class blur_layer;

            // Monotonic timing for input and fades
            using time_point = std::chrono::time_point<std::chrono::steady_clock>;

            std::unique_ptr<font_renderer> font_render;
            std::unique_ptr<image_renderer> image_render;
            std::unique_ptr<simple_renderer> simple_render;
            std::unique_ptr<render::wave_renderer> wave_render;
            std::unique_ptr<render::procedural_wave_renderer> procedural_wave_render;
            std::unique_ptr<openxmb::xmb::MonthlyBackgroundRenderer> monthly_background_render;
            openxmb::xmb::StatusBarRenderer status_bar_render;
            bool monthly_background_failed{};

            vk::UniqueRenderPass backgroundRenderPass, shellRenderPass;
            vk::SampleCountFlagBits renderPassSamples{};

            // Per-frame resolve target for background (offscreen, avoids reusing swapchain mid-frame)
            std::vector<std::unique_ptr<texture>> backgroundResolve;
            std::vector<vk::UniqueImageView> backgroundAttachmentViews,backgroundMsaaViews;
            std::vector<std::unique_ptr<texture>> backgroundDepth;
            std::vector<vk::UniqueFramebuffer> backgroundFramebuffers;

            vk::UniqueDescriptorSetLayout blurDescriptorSetLayout;
            vk::UniquePipelineLayout blurPipelineLayout;
            vk::UniquePipeline blurPipeline;
            vk::UniquePipeline downsamplePipeline;
            vk::UniquePipeline upsamplePipeline;

            struct blur_frame_resources {
                std::unique_ptr<texture> fullSrc;
                std::unique_ptr<texture> fullDst;
                std::unique_ptr<texture> halfSrc;
                std::unique_ptr<texture> halfDst;
                std::unique_ptr<texture> quarterSrc;
                std::unique_ptr<texture> quarterDst;

                vk::DescriptorSet downsampleSet{};
                vk::DescriptorSet halfBlurSet{};
                vk::DescriptorSet upsampleSet{};
                vk::DescriptorSet downsample2Set{};
                vk::DescriptorSet quarterBlurSet{};
                vk::DescriptorSet upsample2Set{};
            };

            std::vector<vk::Image> swapchainImages;
            std::vector<std::unique_ptr<texture>> renderImages;
            std::vector<blur_frame_resources> blurFrames;
            std::vector<vk::UniqueFramebuffer> framebuffers;
            vk::UniqueDescriptorPool blurDescriptorPool;
            std::vector<vk::DescriptorSet> blurDescriptorSets;
            // Extra descriptor pool for downsample/upsample chain
            vk::UniqueDescriptorPool blurExtraDescriptorPool;

            std::unique_ptr<texture> backgroundTexture;
#if OPENXMB_ENABLE_VIDEO_PLAYER
            std::unique_ptr<render::video_background> video_background_render;
            std::filesystem::path background_video_path;
#endif
            // Keep JSON-heavy theme state out of the exported shell BMI.
            struct theme_state;
            std::unique_ptr<theme_state> theme;
            std::string pending_panel;
            std::filesystem::path pending_media;
            main_menu menu{this};
            news_display news{this};
            std::array<std::unique_ptr<texture>, std::to_underlying(action::_length)> buttonTextures;
            std::array<std::string,std::to_underlying(action::_length)> button_labels;
            std::unique_ptr<texture> cursorTexture;
            std::unique_ptr<texture> iconGlassAmbientTexture;
            std::unique_ptr<texture> iconGlassEnvironmentTexture;

            sdl::mix::unique_chunk ok_sound;
            sdl::mix::unique_chunk question_sound;
            sdl::mix::unique_chunk confirm_sound;
            sdl::mix::unique_chunk cancel_sound;
            sdl::mix::unique_chunk back_sound;

            std::chrono::steady_clock::time_point cursor_active_at{};
            glm::vec2 cursor_position{0.5f, 0.5f};
            glm::vec2 cursor_joystick_delta{0.0f, 0.0f};
            glm::ivec2 last_mouse_position{0, 0};
            std::uint32_t last_mouse_buttons = 0;
            bool mouse_state_initialized = false;
            void poll_mouse();
            void tick_cursor();
            bool handle_cursor(const event& event);

            bool fixed_components_loaded = false;
            void create_render_passes();
            void preload_fixed_components();

            void render_gui(gui_renderer& renderer);
            void ensure_blur(int frame);

            // input handling
            constexpr static int controller_axis_input_threshold = 10000;
            std::array<glm::vec2, 2> controller_axis_position;
            std::array<time_point, 2> last_controller_axis_input_time;
            std::array<std::optional<std::tuple<sdl::GameController*, action>>, 2> last_controller_axis_input;
            constexpr static auto controller_axis_input_duration = std::chrono::milliseconds(200);

            time_point last_controller_button_input_time;
            std::optional<std::tuple<sdl::GameController*, sdl::GameControllerButton>> last_controller_button_input;
            constexpr static auto controller_button_input_duration = std::chrono::milliseconds(200);

            // state
            std::string active_audio_device;
            bool desktop_preferences_applied=false;
            bool desktop_defaults_captured=false;
            unsigned desktop_default_flags{};
            int desktop_default_x{},desktop_default_y{},desktop_default_width{},desktop_default_height{};
            std::array<bool,513> held_scancodes{};
            ButtonTiming button_timing;
            time_point last_activity=std::chrono::steady_clock::now();
            bool background_only = false;
            bool ingame_mode = false;

            bool blur_background = false;
            time_point last_blur_background_change;
            float startup_background_blur_px = 0.0f;

            std::vector<std::unique_ptr<app::component>> overlays;

            transition_direction overlay_fade_direction{};
            time_point overlay_fade_time;
            std::unique_ptr<app::component> old_overlay;

            std::optional<clipboard> clipboard;

            // transition duration constants
            constexpr static auto blur_background_transition_duration = std::chrono::milliseconds(500);
            // Slightly longer fade, PS3-like but still snappy
            std::chrono::milliseconds overlay_transition_duration{400};
            float overlay_fade_zoom = 0.85F;

            // Monotonic start time for shader time uniforms (avoid epoch-based float precision loss)
            time_point shader_time_zero { std::chrono::steady_clock::now() };
            openxmb::xmb::BootTimeline startup_timeline;
            std::optional<events::logical_controller_button> startup_dismiss_button;
            std::optional<unsigned int> startup_dismiss_key;
            double startup_elapsed_seconds() const;
    };
}
