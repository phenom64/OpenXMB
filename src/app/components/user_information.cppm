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

#include "../../localization/shell_locale.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

export module openxmb.app:user_information;

import auroreengine;
import glm;
import openxmb.config;
import openxmb.utils;
import :component;
import :dialog_chrome;
import :main;
import :message_overlay;
import :text_entry;
import :xmb_presentation;

export namespace app {

// Avatar colour variants observed in the PS3 icon chooser, in list order after
// the default grey face. Values are sRGB tile colours sampled from the
// captures, used to tint the white user icon; later entries of the scrolling
// list were not captured and are not invented here.
inline const std::array<glm::vec3, 7> user_icon_tints{{
    {0.73F, 0.72F, 0.70F}, {0.90F, 0.28F, 0.22F}, {1.00F, 0.58F, 0.12F}, {1.00F, 0.99F, 0.05F},
    {0.83F, 0.93F, 0.21F}, {0.34F, 0.88F, 0.81F}, {0.31F, 0.81F, 0.97F},
}};

[[nodiscard]] inline std::size_t user_icon_index() {
    const auto value = config::CONFIG.preference("user.icon", "0");
    const auto index = static_cast<std::size_t>(std::max(0, std::atoi(value.c_str())));
    return std::min(index, user_icon_tints.size() - 1);
}

[[nodiscard]] inline std::string user_display_name(std::string_view fallback) {
    const auto stored = config::CONFIG.preference("user.name", "");
    return stored.empty() ? std::string(fallback) : stored;
}

// Loads the user icon once for the information screen and the chooser. The
// compatibility "Create New User" artwork is the avatar face plus a "+"; only
// its face tile (a 206 px square at 83,206 of the 512 px canvas) is shown.
class user_icon_texture {
public:
    void load(auroreengine::resource_loader& loader) {
        if(texture_) return;
        texture_ = std::make_unique<auroreengine::texture>(loader.getDevice(), loader.getAllocator());
        auto path = config::CONFIG.asset_directory / "compat/xmb-ui-compat/icons/xmb_icon_041.png";
        face_only_ = std::filesystem::is_regular_file(path);
        if(!face_only_) path = config::CONFIG.asset_directory / "icons/icon_category_users.png";
        ready_ = loader.loadTexture(texture_.get(), path);
    }
    ~user_icon_texture() { if(ready_.valid()) ready_.wait(); }
    void draw(const xmb_canvas& c, float x, float y, float size, glm::vec3 tint, float alpha) const {
        if(!texture_ || !texture_->loaded) return;
        if(!face_only_) {
            c.image(*texture_, x, y, size, size, dialog_chrome::linear(glm::vec4{tint, alpha}));
            return;
        }
        auto& r = c.renderer;
        const float k = size / 206.0F;
        r.set_clip((c.ox + x * c.scale) / r.frame_size.width, (c.oy + y * c.scale) / r.frame_size.height,
                   size * c.scale / r.frame_size.width, size * c.scale / r.frame_size.height);
        c.image(*texture_, x - 83.0F * k, y - 206.0F * k, 512.0F * k, 512.0F * k, dialog_chrome::linear(glm::vec4{tint, alpha}));
        r.reset_clip();
    }
private:
    std::unique_ptr<auroreengine::texture> texture_;
    std::future<void> ready_;
    bool face_only_{};
};

// Diamond marking the signed-in user, beside the avatar (as in the crossbar).
inline void draw_user_diamond(const xmb_canvas& c, float cx, float cy, float alpha, float scale = 1.0F) {
    // Signed-in marker from the captures: a ~26 px diamond with a bright
    // (~0.83) rim over a darker (~0.58) centre and a soft halo about 8 px wide.
    auto& r = c.renderer;
    const auto n = [&](float x, float y) {
        return glm::vec2{(c.ox + x * c.scale) / r.frame_size.width, (c.oy + y * c.scale) / r.frame_size.height};
    };
    using V = auroreengine::simple_renderer::vertex_data;
    // A diamond is a square turned 45 degrees, so the engine's rounded-square
    // shape (with its edge glows) draws it with antialiased edges. spread is
    // room left outside the shape for an outer glow, in pixels.
    const auto diamond = [&](float half, glm::vec4 colour, float radius, float falloff, float fill, float spread = 0.0F) {
        half *= scale; radius *= scale; falloff *= scale; spread *= scale;
        const float outer = half + spread * std::sqrt(2.0F);
        auroreengine::simple_params p{};
        const float side = outer * std::sqrt(2.0F) * c.scale;
        p.size_px = {side, side};
        p.inset_px = spread * c.scale;
        p.radius_px = std::max(radius * c.scale, 0.001F);
        p.edge_falloff_px = falloff * c.scale;
        p.edge_fill = fill;
        const V top{n(cx, cy - outer), colour, {0, 0}}, right{n(cx + outer, cy), colour, {1, 0}};
        const V left{n(cx - outer, cy), colour, {0, 1}}, bottom{n(cx, cy + outer), colour, {1, 1}};
        r.draw_generic(std::array{top, right, left, right, bottom, left}, p);
    };
    diamond(13.0F, dialog_chrome::light(0.30F, alpha), 1.0F, -2.4F, 0.0F, 11.0F);
    diamond(13.0F, dialog_chrome::linear(glm::vec4{0.58F, 0.58F, 0.57F, alpha}), 1.0F, 0.0F, 1.0F);
    diamond(13.0F, dialog_chrome::light(0.62F, alpha), 1.0F, 1.6F, 0.0F);
}

class user_icon_chooser final : public message_overlay {
public:
    explicit user_icon_chooser(std::function<void()> changed)
        : message_overlay("", ""), changed_(std::move(changed)), current_(user_icon_index()) {}

    result tick(shell* xmb) override {
        icon_.load(*xmb->get_window()->loader);
        if(!pending_ || dialog_chrome::seconds_since(pressed_at_) < std::chrono::duration<float>(dialog_chrome::press_feedback).count())
            return result::success;
        pending_ = false;
        if(on_button_) {
            xmb->emplace_overlay<message_overlay>("Select from [Photo]",
                "Choosing a photo as the user icon is not available yet.");
            return result::success;
        }
        config::CONFIG.preferences["user.icon"] = std::to_string(current_);
        config::CONFIG.save_config();
        if(changed_) changed_();
        return result::success | result::close | result::confirm_sound;
    }

    result on_action(action a) override {
        if(pending_) return result::success;
        switch(a) {
        case action::up:
            if(on_button_ || current_ == 0) return result::success | result::error_rumble;
            --current_; return result::success | result::ok_sound;
        case action::down:
            if(on_button_ || current_ + 1 >= user_icon_tints.size()) return result::success | result::error_rumble;
            ++current_; return result::success | result::ok_sound;
        case action::right:
            if(on_button_) return result::success | result::error_rumble;
            on_button_ = true; return result::success | result::ok_sound;
        case action::left:
            if(!on_button_) return result::success | result::error_rumble;
            on_button_ = false; return result::success | result::ok_sound;
        case action::ok:
            pending_ = true; pressed_at_ = std::chrono::steady_clock::now(); return result::success;
        case action::cancel:
            return result::success | result::close | result::cancel_sound;
        default:
            return result::success;
        }
    }

    // Layout from the 1920x1080 chooser capture: framed current icon, the
    // remaining variants in a column below it, guidance text and a separate
    // push button. The column continues off screen as in the capture.
    void render(auroreengine::gui_renderer& r, shell* xmb) override {
        xmb_canvas c(r);
        const float age = dialog_chrome::seconds_since(opened_);
        const float a = dialog_chrome::smooth01(age / 0.18F);
        dialog_chrome::divider_frame(c, dialog_chrome::smooth01((age - 0.08F) / 0.13F));
        const float press = dialog_chrome::press_amount(pressed_at_);
        if(!on_button_)
            dialog_chrome::avatar_frame(c, 521, 401, 208, dialog_chrome::focus_breath() * (1.0F - 0.75F * press), a);
        icon_.draw(c, 568, 448, 113, user_icon_tints[current_], a);
        for(std::size_t i = current_ + 1, slot = 0; i < user_icon_tints.size(); ++i, ++slot)
            icon_.draw(c, 600, 656 + 80.0F * static_cast<float>(slot), 49, user_icon_tints[i], a);
        c.text("Select an image to use as an icon.", 880, 522, 26, {1, 1, 1, a});
        const auto label = ::openxmb::localization::shell_locale::translate("Select from [Photo]");
        dialog_chrome::push_button(c, r, 1335, 818, std::max(250.0F, dialog_chrome::push_button_width(r, c, label)), label,
            {.focus = on_button_ ? 1.0F : 0.0F, .pressed = on_button_ ? press : 0.0F, .alpha = a});
        xmb->render_message_footer(r, std::array{std::pair{action::ok, "Enter"}, std::pair{action::cancel, "Back"}}, a);
    }

private:
    std::function<void()> changed_;
    std::size_t current_;
    bool on_button_{}, pending_{};
    user_icon_texture icon_;
    std::chrono::steady_clock::time_point opened_{std::chrono::steady_clock::now()}, pressed_at_{};
};

// Users > *User > Triangle > Information: avatar and editable user name.
class user_information_overlay final : public message_overlay {
public:
    user_information_overlay(std::string fallback_name, std::function<void()> changed)
        : message_overlay("", ""), fallback_(std::move(fallback_name)), changed_(std::move(changed)) {}

    result tick(shell* xmb) override {
        icon_.load(*xmb->get_window()->loader);
        if(!pending_ || dialog_chrome::seconds_since(pressed_at_) < std::chrono::duration<float>(dialog_chrome::press_feedback).count())
            return result::success;
        pending_ = false;
        if(name_focused_) {
            xmb->emplace_overlay<text_entry_overlay>("User Name", user_display_name(fallback_), [this](std::string value) {
                config::CONFIG.preferences["user.name"] = value;
                config::CONFIG.save_config();
                if(changed_) changed_();
            }, std::function<void()>{}, keyboard_context::message);
        } else {
            xmb->push_overlay(std::make_unique<user_icon_chooser>(changed_));
        }
        return result::success | result::confirm_sound;
    }

    result on_action(action a) override {
        if(pending_) return result::success;
        switch(a) {
        case action::up:
            if(!name_focused_) return result::success | result::error_rumble;
            name_focused_ = false; return result::success | result::ok_sound;
        case action::down:
            if(name_focused_) return result::success | result::error_rumble;
            name_focused_ = true; return result::success | result::ok_sound;
        case action::ok:
            pending_ = true; pressed_at_ = std::chrono::steady_clock::now(); return result::success;
        case action::cancel:
            return result::success | result::close | result::cancel_sound;
        default:
            return result::success;
        }
    }

    // Positions from the user-name capture: diamond, 112 px avatar, "User"
    // caption and the bordered value box beneath; both controls focus.
    void render(auroreengine::gui_renderer& r, shell* xmb) override {
        xmb_canvas c(r);
        const float age = dialog_chrome::seconds_since(opened_);
        const float a = dialog_chrome::smooth01((age - 0.03F) / 0.15F);
        dialog_chrome::divider_frame(c, dialog_chrome::smooth01((age - 0.12F) / 0.13F));
        const float press = dialog_chrome::press_amount(pressed_at_);
        draw_user_diamond(c, 693.5F, 351.5F, a);
        if(!name_focused_)
            dialog_chrome::avatar_frame(c, 720, 248, 207, dialog_chrome::focus_breath() * (1.0F - 0.75F * press), a);
        icon_.draw(c, 768, 296, 112, user_icon_tints[user_icon_index()], a);
        const auto name = user_display_name(fallback_);
        const float pixels = 24 * c.scale * config::CONFIG.textScale;
        const float value_width = std::max(60.0F, r.text_width_pixels(name, pixels) / c.scale + 12.0F);
        c.text("User", 755, 516, 24, {1, 1, 1, a});
        const float caption = r.text_width_pixels(::openxmb::localization::shell_locale::translate("User"), pixels) / c.scale;
        const float box_x = 755 + caption + 33;
        dialog_chrome::value_box(c, box_x, 492, value_width, 31, {.focus = name_focused_ ? 1.0F : 0.0F,
            .pressed = name_focused_ ? press : 0.0F, .alpha = a});
        c.text(name, box_x + 6, 516, 24, {1, 1, 1, a});
        xmb->render_message_footer(r, std::array{std::pair{action::ok, "Edit"}, std::pair{action::cancel, "Back"}},
            dialog_chrome::smooth01(age / 0.12F));
    }

private:
    std::string fallback_;
    std::function<void()> changed_;
    bool name_focused_{}, pending_{};
    user_icon_texture icon_;
    std::chrono::steady_clock::time_point opened_{std::chrono::steady_clock::now()}, pressed_at_{};
};

}
