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
#include <string_view>

export module openxmb.app:dialog_chrome;

import auroreengine;
import glm;
import openxmb.config;
import :xmb_presentation;

// Shared message-screen chrome: dividers, push buttons, text fields and focus
// frames. Geometry and translucency come from 1920x1080 RPCS3 captures of the
// playlist name message, the user information screen and the icon chooser.
// They are single-trial visual estimates, not calibrated firmware constants.
export namespace app::dialog_chrome {

inline constexpr float divider_top_y = 199.0F;
inline constexpr float divider_bottom_y = 879.0F;
inline constexpr float prompt_baseline_y = 324.0F;
inline constexpr float prompt_size = 26.0F;
inline constexpr float field_x = 556.0F;
inline constexpr float field_y = 480.0F;
inline constexpr float field_width = 808.0F;
inline constexpr float field_height = 40.0F;
inline constexpr float field_text_size = 24.0F;
inline constexpr float button_height = 44.0F;
inline constexpr float button_text_size = 24.0F;
inline constexpr float button_min_width = 60.0F;
inline constexpr float button_padding = 15.0F;
inline constexpr float button_gap = 34.0F;
inline constexpr float field_to_button_gap = 34.0F;

// Message cross-fade observed at roughly a quarter second (one partly
// dropped-frame trial); kept as one tunable constant.
inline constexpr auto message_fade = std::chrono::milliseconds(250);
// Activation feedback: the focused field lost its highlight for a few frames
// before the keyboard appeared (single observation).
inline constexpr auto press_feedback = std::chrono::milliseconds(140);

struct widget_state {
    float focus = 0.0F;   // 0 = idle, 1 = focused
    float pressed = 0.0F; // 0..1 activation feedback
    float alpha = 1.0F;
};

[[nodiscard]] inline float seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
}

[[nodiscard]] inline float smooth01(float value) {
    value = std::clamp(value, 0.0F, 1.0F);
    return value * value * (3.0F - 2.0F * value);
}

// Focus frames brighten and relax slowly. The captured avatar frame visibly
// varied between samples; the period was not measured, so this reuses the
// crossbar label pulse rhythm instead of inventing a new one.
[[nodiscard]] inline float focus_breath() {
    return std::clamp((xmb_canvas::pulse() - 0.7F) / 0.55F, 0.0F, 1.0F);
}

[[nodiscard]] inline float press_amount(std::chrono::steady_clock::time_point pressed_at) {
    if(pressed_at.time_since_epoch().count() == 0) {
        return 0.0F;
    }
    const float t = seconds_since(pressed_at) /
        std::chrono::duration<float>(press_feedback).count();
    return t >= 1.0F ? 0.0F : 1.0F - smooth01(t);
}

// The captures are gamma-encoded (sRGB) and the PS3 blends in that space,
// while this renderer blends in linear light on an sRGB target. Measured
// colours and alphas are written as sRGB values and converted here so the
// composited result matches the capture.
[[nodiscard]] inline float linear(float srgb) {
    return std::pow(std::clamp(srgb, 0.0F, 1.0F), 2.2F);
}

[[nodiscard]] inline glm::vec4 linear(glm::vec4 srgb) {
    return {linear(srgb.r), linear(srgb.g), linear(srgb.b), srgb.a};
}

// Black overlay: an sRGB darkening factor is independent of the background.
[[nodiscard]] inline float shade_alpha(float srgb_alpha) {
    return 1.0F - std::pow(1.0F - std::clamp(srgb_alpha, 0.0F, 1.0F), 2.2F);
}

// White overlay: the equivalent depends on the background, so it is matched
// at the dark theme backgrounds the captures were measured on (~23% grey).
[[nodiscard]] inline float light_alpha(float srgb_alpha) {
    constexpr float background = 0.227F;
    const float base = linear(background);
    const float target = linear(background + std::clamp(srgb_alpha, 0.0F, 1.0F) * (1.0F - background));
    return (target - base) / (1.0F - base);
}

[[nodiscard]] inline glm::vec4 light(float srgb_alpha, float alpha = 1.0F) {
    return {1, 1, 1, light_alpha(srgb_alpha) * alpha};
}

inline void divider_frame(const xmb_canvas& c, float alpha) {
    c.rect(0, divider_top_y, 1920, 1, light(0.70F, alpha));
    c.rect(0, divider_bottom_y, 1920, 1, light(0.70F, alpha));
}

// Translucent box used for text fields, value boxes and the avatar frame
// (sRGB alphas): a two-pixel rim with a softer outer row, a faint fill and
// soft sheens along the inside top and bottom edges.
inline void glass_box(const xmb_canvas& c, float x, float y, float w, float h,
                      float rim, float fill, float sheen, float alpha) {
    const glm::vec4 clear{1, 1, 1, 0};
    const auto outer = light(rim * 0.65F, alpha), inner = light(rim, alpha);
    c.rect(x, y, w, 1, outer);
    c.rect(x, y + h - 1, w, 1, outer);
    c.rect(x, y + 1, 1, h - 2, outer);
    c.rect(x + w - 1, y + 1, 1, h - 2, outer);
    c.rect(x + 1, y + 1, w - 2, 1, inner);
    c.rect(x + 1, y + h - 2, w - 2, 1, inner);
    c.rect(x + 1, y + 2, 1, h - 4, inner);
    c.rect(x + w - 2, y + 2, 1, h - 4, inner);
    c.rect(x + 2, y + 2, w - 4, h - 4, light(fill, alpha));
    c.gradient(x + 2, y + 2, w - 4, h * 0.3F, light(sheen, alpha), clear);
    c.gradient(x + 2, y + h * 0.78F, w - 4, h * 0.22F - 2, clear, light(sheen * 1.4F, alpha));
}

// The focused name field's rim breathed between ~0.13 and ~0.42 sRGB alpha
// over successive captured frames; the unfocused look was not captured.
inline void text_box(const xmb_canvas& c, float x, float y, float w, float h, widget_state s) {
    const float focus = std::clamp(s.focus, 0.0F, 1.0F) * (1.0F - 0.75F * s.pressed);
    const float breath = focus * focus_breath();
    glass_box(c, x, y, w, h, 0.10F + 0.03F * focus + 0.29F * breath, 0.015F + 0.05F * breath,
        0.03F + 0.05F * breath, s.alpha);
}

inline void text_field(const xmb_canvas& c, auroreengine::gui_renderer& r, float x, float y, float w, float h,
                       std::string_view value, widget_state s) {
    text_box(c, x, y, w, h, s);
    const float pixels = field_text_size * c.scale * config::CONFIG.textScale;
    const float inset = 7.0F;
    const auto metrics = r.text_line_metrics_pixels(pixels) / c.scale;
    const float baseline = y + h * 0.5F + (metrics.x - metrics.y) * 0.5F;
    r.set_clip((c.ox + (x + 2) * c.scale) / r.frame_size.width, (c.oy + y * c.scale) / r.frame_size.height,
               (w - 4) * c.scale / r.frame_size.width, h * c.scale / r.frame_size.height);
    c.text(value, x + inset, baseline, field_text_size, {1, 1, 1, s.alpha});
    r.reset_clip();
}

[[nodiscard]] inline float push_button_width(auroreengine::gui_renderer& r, const xmb_canvas& c, std::string_view label) {
    const float pixels = button_text_size * c.scale * config::CONFIG.textScale;
    return std::max(button_min_width, r.text_width_pixels(label, pixels) / c.scale + 2.0F * button_padding);
}

// Push button, following the vertical sRGB alpha profile of the unfocused
// playlist OK button: bright top edge, a glossy band easing into a flat
// middle, a brightening lower part and a bright bottom edge. The focused and
// pressed treatments were not captured; they are inferred from the focused
// text field (a brighter, slowly breathing body and glow) and a dimmed press.
inline void push_button(const xmb_canvas& c, auroreengine::gui_renderer& r, float x, float y, float w,
                        std::string_view label, widget_state s) {
    const float a = s.alpha;
    const float press = std::clamp(s.pressed, 0.0F, 1.0F);
    const float focus = std::clamp(s.focus, 0.0F, 1.0F);
    const float lift = focus * (0.06F + 0.06F * focus_breath()) * (1.0F - press);
    const float gloss = 1.0F - 0.35F * press;
    const auto body = [&](float v) { return light((v + lift) * gloss, a); };
    if(focus > 0.0F) {
        c.rect(x - 4, y - 4, w + 8, button_height + 8, light(lift * 0.9F, a), 7);
        c.rect(x - 2, y - 2, w + 4, button_height + 4, light(lift * 1.6F, a), 5);
    }
    c.rect(x + 2, y, w - 4, 1, body(0.62F));
    c.gradient(x, y + 1, w, 7, body(0.50F), body(0.50F));
    c.gradient(x, y + 8, w, 5, body(0.50F), body(0.33F));
    c.gradient(x, y + 13, w, 18, body(0.33F), body(0.37F));
    c.gradient(x, y + 31, w, 8, body(0.37F), body(0.43F));
    c.gradient(x, y + 39, w, 4, body(0.45F), body(0.62F));
    c.rect(x + 2, y + 43, w - 4, 1, body(0.66F));
    const float pixels = button_text_size * c.scale * config::CONFIG.textScale;
    const auto metrics = r.text_line_metrics_pixels(pixels) / c.scale;
    const float baseline = y + button_height * 0.5F + (metrics.x - metrics.y) * 0.5F;
    c.text(label, x + w * 0.5F, baseline, button_text_size, {1, 1, 1, a}, 0, true);
}

// A horizontal row of push buttons centred on cx; an out-of-range focused or
// pressed index leaves every button idle.
inline void push_button_row(const xmb_canvas& c, auroreengine::gui_renderer& r, float cx, float y,
                            const auto& labels, std::size_t focused, std::size_t pressed_index,
                            float pressed, float alpha) {
    float total = 0;
    for(const auto& label : labels) total += push_button_width(r, c, label);
    if(!labels.empty()) total += button_gap * static_cast<float>(labels.size() - 1);
    float x = cx - total * 0.5F;
    std::size_t index = 0;
    for(const auto& label : labels) {
        const float w = push_button_width(r, c, label);
        push_button(c, r, x, y, w, label, {
            .focus = index == focused ? 1.0F : 0.0F,
            .pressed = index == pressed_index ? pressed : 0.0F,
            .alpha = alpha});
        x += w + button_gap;
        ++index;
    }
}

}
