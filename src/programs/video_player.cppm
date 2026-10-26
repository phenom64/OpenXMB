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
#include <openxmb/video.hpp>
#include <openxmb/video_worker.hpp>
#include <openxmb/music/player.hpp>

export module openxmb.app:video_player;

import dreamrender;
import glm;
import spdlog;
import vulkan_hpp;
import vma;
import i18n;
import openxmb.utils;
import :component;
import :programs;
import :message_overlay;
import :base_viewer;
import openxmb.app;

namespace programs {
using namespace app;
using namespace mfk::i18n::literals;
namespace video = openxmb::video;

export class video_player : private base_viewer, public component, public action_receiver,
                            public event_receiver, public joystick_receiver, public mouse_receiver {
public:
    float typography_scale() const override { return 2.5F; }

    video_player(std::filesystem::path file, dreamrender::resource_loader& loader)
        : device(loader.getDevice()), allocator(loader.getAllocator()) {
        image_width = image_height = 1;
        openxmb::music::service().set_external_duck(true);
        playback = std::make_unique<video::worker>();
        discover_playlist(file);
        open_path(std::move(file));
    }

    ~video_player() {
        playback.reset();
        if(texture) { try { wait_gpu(); } catch(...) {} }
        staging_buffers.clear();
        staging_allocations.clear();
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
            playback->set_volume(volume);
            playback->set_repeat(repetition == repeat_mode::file);
            if(!snapshot->info.audio_tracks.empty() && snapshot->info.audio_status != "Audio output active") status_message = snapshot->info.audio_status;
        }
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
        if(!texture || texture->width != frame.width || texture->height != frame.height || staging_buffers.size() != xmb->get_window()->swapchainImageCount) {
            // Resolution changes and file transitions retire resources only
            // after submitted draws have stopped referencing their image views.
            wait_gpu();
            texture.reset();
            staging_buffers.clear();
            staging_allocations.clear();
            texture = std::make_unique<dreamrender::texture>(device, allocator, frame.width, frame.height);
            std::size_t count = xmb->get_window()->swapchainImageCount;
            staging_allocations.resize(count);
            staging_buffers.resize(count);
            vk::BufferCreateInfo buffer_info({}, frame.rgba.size(), vk::BufferUsageFlagBits::eTransferSrc, vk::SharingMode::eExclusive);
            vma::AllocationCreateInfo allocation_info({}, vma::MemoryUsage::eCpuToGpu);
            for(std::size_t i = 0; i < count; ++i) std::tie(staging_buffers[i], staging_allocations[i]) = allocator.createBufferUnique(buffer_info, allocation_info);
            image_initialized = false;
        }
        if(frame_index < 0 || static_cast<std::size_t>(frame_index) >= staging_buffers.size()) return;
        allocator.copyMemoryToAllocation(frame.rgba.data(), staging_allocations[frame_index].get(), 0, frame.rgba.size());
        vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        cmd.pipelineBarrier(image_initialized ? vk::PipelineStageFlagBits::eFragmentShader : vk::PipelineStageFlagBits::eTopOfPipe,
            vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
            vk::ImageMemoryBarrier(image_initialized ? vk::AccessFlags{vk::AccessFlagBits::eShaderRead} : vk::AccessFlags{}, vk::AccessFlagBits::eTransferWrite,
                image_initialized ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, texture->image, range));
        cmd.copyBufferToImage(staging_buffers[frame_index].get(), texture->image, vk::ImageLayout::eTransferDstOptimal,
            vk::BufferImageCopy(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1), vk::Offset3D(0, 0, 0),
                vk::Extent3D(static_cast<uint32_t>(frame.width), static_cast<uint32_t>(frame.height), 1)));
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {},
            vk::ImageMemoryBarrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, texture->image, range));
        image_initialized = true;
        image_height = frame.height;
        image_width = static_cast<unsigned int>(std::max(1.0, frame.aspect_ratio * frame.height));
        pending_frame.reset();
    }

    void render(dreamrender::gui_renderer& renderer, shell* xmb) override {
        renderer.draw_rect({0, 0}, {1, 1}, {0, 0, 0, 1});
        if(!loaded) {
            renderer.draw_text("Opening video…"_(), 0.5f, 0.5f, 0.025f, glm::vec4(1), true, true);
            return;
        }
        if(texture && image_initialized) {
            if(screen == screen_mode::stretch) renderer.draw_image(texture->imageView.get(), 0, 0, renderer.aspect_ratio, 1);
            else if(screen == screen_mode::original) {
                float h = std::min(1.0f, static_cast<float>(texture->height) / std::max(1u, xmb->get_window()->swapchainExtent.height));
                float w = h * static_cast<float>(playback->info().aspect_ratio);
                if(w > renderer.aspect_ratio) { h *= renderer.aspect_ratio / w; w = renderer.aspect_ratio; }
                renderer.draw_image(texture->imageView.get(), (1 - w / renderer.aspect_ratio) / 2, (1 - h) / 2, w, h);
            } else {
                float saved_zoom = zoom;
                if(screen == screen_mode::fit) zoom = 1;
                base_viewer::render(texture->imageView.get(), 1.0f, renderer);
                zoom = saved_zoom;
            }
        }
        draw_subtitles(renderer);
        bool controls = menu != menu_kind::none || playback->playback_state() != video::state::playing ||
            std::chrono::steady_clock::now() - last_input < std::chrono::seconds(4);
        if(controls) {
            if(!status_message.empty()) renderer.draw_text(status_message, 0.05f, 0.06f, 0.016f);
            renderer.draw_rect({0, 0.87f}, {1, 0.13f}, {0, 0, 0, 0.8f});
            float progress = playback->info().duration > 0 ? static_cast<float>(std::clamp(playback->position() / playback->info().duration, 0.0, 1.0)) : 0;
            renderer.draw_rect({0.05f, 0.885f}, {0.90f, 0.004f}, {0.3f, 0.3f, 0.3f, 1});
            if(progress > 0) renderer.draw_rect({0.05f, 0.885f}, {0.90f * progress, 0.004f}, {0.72f, 0.81f, 0.96f, 1});
            renderer.draw_text(path.filename().string(), 0.05f, 0.915f, 0.016f);
            auto position_text = time_label(playback->position()) + " / " + time_label(playback->info().duration) + "   " + std::to_string(static_cast<int>(volume * 100)) + "%";
            renderer.draw_text(position_text, 0.95f - renderer.measure_text(position_text, 0.016f).x, 0.915f, 0.016f);
            render_controller_buttons(xmb, renderer, 0.5f, 0.965f, std::array{
                std::pair{action::ok, playback->playback_state() == video::state::playing ? std::string_view{"Pause"} : std::string_view{"Play"}},
                std::pair{action::left, std::string_view{"−10s"}}, std::pair{action::right, std::string_view{"+10s"}},
                std::pair{action::options, std::string_view{"Controls"}}, std::pair{action::cancel, std::string_view{"Close"}}
            });
        }
        if(menu != menu_kind::none) draw_menu(renderer);
        if(show_information) {
            const auto& info = playback->info();
            renderer.draw_rect({0.03f, 0.05f}, {0.49f, 0.18f}, {0, 0, 0, 0.8f});
            renderer.draw_text(std::to_string(info.width) + " × " + std::to_string(info.height) + " · " + utils::to_fixed_string(info.frame_rate, 2) + " fps", 0.05f, 0.09f, 0.018f);
            renderer.draw_text(info.decoder, 0.05f, 0.13f, 0.016f);
            renderer.draw_text(info.audio_status, 0.05f, 0.17f, 0.016f);
            if(playback->rate() != 1.0) renderer.draw_text("Audio is muted during speed playback"_(), 0.05f, 0.21f, 0.014f);
        }
    }

    result on_action(action command) override {
        last_input = std::chrono::steady_clock::now();
        if(menu != menu_kind::none) return menu_action(command);
        if(command == action::cancel) return result::close;
        if(!loaded) return result::success;
        switch(command) {
            case action::ok: playback->pause(playback->playback_state() == video::state::playing); break;
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
        if(auto* key = event.get<events::key_down>(); key && loaded) {
            switch(key->keycode) {
                case SDL_SCANCODE_SPACE: return on_action(action::ok);
                case SDL_SCANCODE_N: advance_file(1); return result::success;
                case SDL_SCANCODE_P: advance_file(-1); return result::success;
                case SDL_SCANCODE_PERIOD: playback->step(1); return result::success;
                case SDL_SCANCODE_COMMA: playback->step(-1); return result::success;
                case SDL_SCANCODE_F: screen = static_cast<screen_mode>((static_cast<int>(screen) + 1) % 4); return result::success;
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

private:
    enum class repeat_mode { off, file, all };
    enum class screen_mode { fit, original, zoom, stretch };
    enum class menu_kind { none, main, audio, subtitles, screen, speed, scenes, files };
    vk::Device device;
    vma::Allocator allocator;
    std::filesystem::path path;
    std::vector<std::filesystem::path> playlist;
    std::size_t playlist_index = 0;
    dreamrender::window* window = nullptr;
    bool loaded = false;
    std::unique_ptr<video::worker> playback;
    std::optional<video::frame> pending_frame;
    std::unique_ptr<dreamrender::texture> texture;
    // Buffers must be destroyed before their backing allocations.
    std::vector<vma::UniqueAllocation> staging_allocations;
    std::vector<vma::UniqueBuffer> staging_buffers;
    bool image_initialized = false;
    bool end_handled = false;
    float volume = 1;
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

    static bool video_file(const std::filesystem::path& file) {
        auto ext = file.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        constexpr std::array<std::string_view, 10> types{ ".mp4", ".mkv", ".avi", ".mov", ".webm", ".mpg", ".mpeg", ".m4v", ".ts", ".ogv" };
        return std::find(types.begin(), types.end(), ext) != types.end();
    }

    void discover_playlist(const std::filesystem::path& current) {
        std::error_code error;
        auto directory = current.has_parent_path() ? current.parent_path() : std::filesystem::current_path(error);
        for(std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, error), end; it != end && !error; it.increment(error)) {
            if(it->is_regular_file(error) && video_file(it->path())) playlist.push_back(it->path());
        }
        if(std::find(playlist.begin(), playlist.end(), current) == playlist.end()) playlist.push_back(current);
        std::sort(playlist.begin(), playlist.end());
        playlist.erase(std::unique(playlist.begin(), playlist.end()), playlist.end());
        playlist_index = static_cast<std::size_t>(std::find(playlist.begin(), playlist.end(), current) - playlist.begin());
    }

    void open_path(std::filesystem::path next) {
        loaded = false;
        status_message.clear();
        if(texture) wait_gpu();
        texture.reset();
        staging_buffers.clear();
        staging_allocations.clear();
        path = std::move(next);
        pending_frame.reset();
        image_initialized = false;
        end_handled = false;
        menu = menu_kind::none;
        video::options settings;
        settings.hardware_acceleration = OPENXMB_HWACCEL_STR;
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
        if(playback) playback->set_volume(volume);
        last_input = std::chrono::steady_clock::now();
    }

    void draw_subtitles(dreamrender::gui_renderer& renderer) {
        auto text = playback->subtitle_text();
        if(text.empty()) return;
        std::vector<std::string> lines;
        std::size_t offset = 0;
        while(offset < text.size() && lines.size() < 5) {
            auto end = text.find('\n', offset);
            auto line = text.substr(offset, end == std::string::npos ? end : end - offset);
            // Bound long dialogue to the available screen width without dropping
            // trailing text. FFmpeg has already stripped ASS/HTML formatting.
            while(line.size() > 75 && lines.size() < 5) {
                auto split = line.rfind(' ', 75);
                if(split == std::string::npos || split == 0) split = 75;
                lines.push_back(line.substr(0, split));
                line.erase(0, split + (line[split] == ' '));
            }
            if(lines.size() < 5) lines.push_back(std::move(line));
            if(end == std::string::npos) break;
            offset = end + 1;
        }
        float y = 0.83f - static_cast<float>(lines.size()) * 0.035f;
        for(const auto& line : lines) {
            renderer.draw_text(line, 0.502f, y + 0.002f, 0.025f, {0, 0, 0, 1}, true, true);
            renderer.draw_text(line, 0.5f, y, 0.025f, glm::vec4(1), true, true);
            y += 0.035f;
        }
    }

    void open_menu(menu_kind kind) {
        menu = kind;
        selection = 0;
        last_input = std::chrono::steady_clock::now();
        if(kind == menu_kind::scenes) {
            scene_return = playback->position();
            scene_was_paused = playback->playback_state() != video::state::playing;
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
                std::string("Repeat: ") + (repetition == repeat_mode::off ? "Off" : repetition == repeat_mode::file ? "This video" : "All videos"),
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

    void draw_menu(dreamrender::gui_renderer& renderer) {
        auto rows = menu_rows();
        renderer.draw_rect({0.57f, 0.035f}, {0.40f, 0.81f}, {0.025f, 0.035f, 0.06f, 0.94f});
        const auto title = menu == menu_kind::scenes ? "Scene search"_() :
            menu == menu_kind::subtitles ? "Subtitles"_() :
            menu == menu_kind::audio ? "Audio tracks"_() : "Video controls"_();
        renderer.draw_text(title, 0.60f, 0.08f, 0.030f);
        renderer.set_clip(0.585f, 0.105f, 0.37f, 0.70f);
        std::size_t first = selection > 6 ? selection - 6 : 0;
        if(rows.size() > 14) first = std::min(first, rows.size() - 14);
        for(std::size_t i = first; i < rows.size() && i < first + 14; ++i) {
            float y = 0.13f + static_cast<float>(i - first) * 0.047f;
            if(i == selection) renderer.draw_rect({0.585f, y - 0.021f}, {0.37f, 0.041f}, {0.18f, 0.27f, 0.43f, 0.98f});
            renderer.draw_text(rows[i], 0.605f, y, 0.024f);
        }
        renderer.reset_clip();
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
        if(command == action::up || command == action::down) {
            selection = command == action::up ? (selection + rows.size() - 1) % rows.size() : (selection + 1) % rows.size();
            if(menu == menu_kind::scenes) preview_scene();
            return result::success;
        }
        if(command != action::ok) return result::success;
        if(menu == menu_kind::main) {
            switch(selection) {
                case 0: playback->pause(playback->playback_state() == video::state::playing); menu = menu_kind::none; break;
                case 1: playback->replay(); menu = menu_kind::none; break;
                case 2: playback->seek(0); playback->pause(false); menu = menu_kind::none; break;
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
