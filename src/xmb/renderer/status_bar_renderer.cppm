module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <string_view>

#include "openxmb/xmb/status_bar.hpp"

export module openxmb.xmb.status_bar_renderer;

import dreamrender;
import glm;

export namespace openxmb::xmb {

class StatusBarRenderer {
public:
  void render(dreamrender::gui_renderer &renderer, const StatusBarTime &time,
              float opacity = 1.0F) const;

  // Convenience path for production. Deterministic tests and captures should
  // resolve their fixed civil time once and use the overload above.
  [[nodiscard]] bool
  render_local(dreamrender::gui_renderer &renderer,
               std::chrono::system_clock::time_point time,
               float opacity = 1.0F) const;
};

} // namespace openxmb::xmb

namespace openxmb::xmb {
namespace {

struct ContainedLayout {
  float scale{};
  float offset_x{};
  float offset_y{};
  float framebuffer_width{};
  float framebuffer_height{};

  [[nodiscard]] float x(float logical_x) const noexcept {
    return (offset_x + logical_x * scale) / framebuffer_width;
  }
  [[nodiscard]] float y(float logical_y) const noexcept {
    return (offset_y + logical_y * scale) / framebuffer_height;
  }
  [[nodiscard]] float width(float logical_width) const noexcept {
    return logical_width * scale / framebuffer_width;
  }
  [[nodiscard]] float height(float logical_height) const noexcept {
    return logical_height * scale / framebuffer_height;
  }
};

[[nodiscard]] ContainedLayout
make_layout(const dreamrender::gui_renderer &renderer) noexcept {
  const auto width = static_cast<float>(renderer.frame_size.width);
  const auto height = static_cast<float>(renderer.frame_size.height);
  const auto scale =
      std::min(width / StatusBarMetrics::logical_width,
               height / StatusBarMetrics::logical_height);
  return {
      .scale = scale,
      .offset_x =
          (width - StatusBarMetrics::logical_width * scale) * 0.5F,
      .offset_y =
          (height - StatusBarMetrics::logical_height * scale) * 0.5F,
      .framebuffer_width = width,
      .framebuffer_height = height,
  };
}

void draw_logical_rect(dreamrender::gui_renderer &renderer,
                       const ContainedLayout &layout, float x, float y,
                       float width, float height, glm::vec4 color,
                       dreamrender::simple_params parameters = {}) {
  renderer.draw_rect({layout.x(x), layout.y(y)},
                     {layout.width(width), layout.height(height)}, color,
                     parameters);
}

void draw_u_frame(dreamrender::gui_renderer &renderer,
                  const ContainedLayout &layout, float opacity) {
  constexpr std::array glow_alpha{0.13F, 0.095F, 0.065F, 0.035F, 0.02F};
  for (std::size_t index = 0; index < glow_alpha.size(); ++index) {
    const auto inset = static_cast<float>(index + 1);
    const auto alpha = glow_alpha[index] * opacity;
    const auto x = StatusBarMetrics::frame_x + inset;
    const auto y = StatusBarMetrics::frame_y + inset;
    const auto width = StatusBarMetrics::frame_width + 4.0F - inset -
                       StatusBarMetrics::frame_corner_radius;
    const auto height = StatusBarMetrics::frame_height - inset * 2.0F;
    draw_logical_rect(renderer, layout,
                      x + StatusBarMetrics::frame_corner_radius, y, width,
                      1.0F, {1.0F, 1.0F, 1.0F, alpha});
    draw_logical_rect(renderer, layout, x, y + StatusBarMetrics::frame_corner_radius,
                      1.0F,
                      height - StatusBarMetrics::frame_corner_radius * 2.0F,
                      {1.0F, 1.0F, 1.0F, alpha});
    draw_logical_rect(renderer, layout,
                      x + StatusBarMetrics::frame_corner_radius,
                      y + height - 1.0F, width, 1.0F,
                      {1.0F, 1.0F, 1.0F, alpha});
  }

  constexpr auto border_alpha = 0.175F;
  constexpr auto horizontal_rule_width =
      StatusBarMetrics::frame_width + 4.0F -
      StatusBarMetrics::frame_corner_radius;
  draw_logical_rect(renderer, layout,
                    StatusBarMetrics::frame_x +
                        StatusBarMetrics::frame_corner_radius,
                    StatusBarMetrics::frame_y, horizontal_rule_width, 1.0F,
                    {1.0F, 1.0F, 1.0F, border_alpha * opacity});
  draw_logical_rect(renderer, layout, StatusBarMetrics::frame_x,
                    StatusBarMetrics::frame_y +
                        StatusBarMetrics::frame_corner_radius,
                    1.0F,
                    StatusBarMetrics::frame_height -
                        StatusBarMetrics::frame_corner_radius * 2.0F,
                    {1.0F, 1.0F, 1.0F, border_alpha * opacity});
  draw_logical_rect(renderer, layout,
                    StatusBarMetrics::frame_x +
                        StatusBarMetrics::frame_corner_radius,
                    StatusBarMetrics::frame_y +
                        StatusBarMetrics::frame_height - 1.0F,
                    horizontal_rule_width, 1.0F,
                    {1.0F, 1.0F, 1.0F, border_alpha * opacity});
}

constexpr std::size_t kRingSegments = 32;

void draw_ring(dreamrender::gui_renderer &renderer,
               const ContainedLayout &layout, float center_x, float center_y,
               float radius, float width, glm::vec4 color) {
  std::array<dreamrender::simple_renderer::vertex_data, kRingSegments * 6>
      vertices{};
  constexpr auto pi = 3.14159265358979323846F;
  constexpr auto full_turn = 2.0F * pi;
  const auto inner = std::max(0.0F, radius - width * 0.5F);
  const auto outer = radius + width * 0.5F;

  const auto vertex = [&](float x, float y) {
    return dreamrender::simple_renderer::vertex_data{
        .position = {layout.x(x), layout.y(y)},
        .color = color,
        .tex_coords = {},
    };
  };
  for (std::size_t segment = 0; segment < kRingSegments; ++segment) {
    const auto angle0 = full_turn * static_cast<float>(segment) /
                        static_cast<float>(kRingSegments);
    const auto angle1 = full_turn * static_cast<float>(segment + 1) /
                        static_cast<float>(kRingSegments);
    const auto cos0 = std::cos(angle0);
    const auto sin0 = std::sin(angle0);
    const auto cos1 = std::cos(angle1);
    const auto sin1 = std::sin(angle1);
    const auto inner0 = vertex(center_x + cos0 * inner,
                               center_y + sin0 * inner);
    const auto outer0 = vertex(center_x + cos0 * outer,
                               center_y + sin0 * outer);
    const auto inner1 = vertex(center_x + cos1 * inner,
                               center_y + sin1 * inner);
    const auto outer1 = vertex(center_x + cos1 * outer,
                               center_y + sin1 * outer);
    const auto base = segment * 6;
    vertices[base + 0] = inner0;
    vertices[base + 1] = outer0;
    vertices[base + 2] = inner1;
    vertices[base + 3] = outer0;
    vertices[base + 4] = outer1;
    vertices[base + 5] = inner1;
  }
  renderer.draw_generic(std::span{vertices});
}

void draw_hand(dreamrender::gui_renderer &renderer,
               const ContainedLayout &layout, float center_x, float center_y,
               float angle, float length, float width, glm::vec4 color) {
  const auto dx = std::cos(angle);
  const auto dy = std::sin(angle);
  const auto px = -dy * width * 0.5F;
  const auto py = dx * width * 0.5F;
  const auto end_x = center_x + dx * length;
  const auto end_y = center_y + dy * length;
  const auto vertex = [&](float x, float y) {
    return dreamrender::simple_renderer::vertex_data{
        .position = {layout.x(x), layout.y(y)},
        .color = color,
        .tex_coords = {},
    };
  };
  const std::array vertices{
      vertex(center_x + px, center_y + py),
      vertex(center_x - px, center_y - py),
      vertex(end_x + px, end_y + py),
      vertex(center_x - px, center_y - py),
      vertex(end_x - px, end_y - py),
      vertex(end_x + px, end_y + py),
  };
  renderer.draw_generic(vertices);
}

void draw_clock_face(dreamrender::gui_renderer &renderer,
                     const ContainedLayout &layout,
                     const ClockHandAngles &angles, float x_offset,
                     float y_offset, float ring_width, float hand_width,
                     glm::vec4 color) {
  const auto center_x = StatusBarMetrics::icon_center_x + x_offset;
  const auto center_y = StatusBarMetrics::icon_center_y + y_offset;
  draw_ring(renderer, layout, center_x, center_y,
            StatusBarMetrics::icon_radius - ring_width * 0.5F - 0.6F,
            ring_width, color);
  draw_hand(renderer, layout, center_x, center_y, angles.hour_radians,
            StatusBarMetrics::icon_radius * 0.47F, hand_width, color);
  draw_hand(renderer, layout, center_x, center_y, angles.minute_radians,
            StatusBarMetrics::icon_radius * 0.69F, hand_width, color);
}

} // namespace

void StatusBarRenderer::render(dreamrender::gui_renderer &renderer,
                               const StatusBarTime &time,
                               float opacity) const {
  opacity = std::clamp(opacity, 0.0F, 1.0F);
  if (opacity <= 0.001F || !valid_status_bar_time(time)) {
    return;
  }

  std::array<char, 16> text_buffer{};
  const auto text_size = format_status_bar_time(time, text_buffer);
  if (text_size == 0) {
    return;
  }
  const std::string_view time_text(text_buffer.data(), text_size);
  const auto layout = make_layout(renderer);

  dreamrender::simple_params panel_parameters{};
  panel_parameters.border_radius = {
      StatusBarMetrics::frame_corner_radius /
          StatusBarMetrics::frame_height,
      0.0F,
      0.0F,
      StatusBarMetrics::frame_corner_radius /
          StatusBarMetrics::frame_height,
  };
  draw_logical_rect(renderer, layout, StatusBarMetrics::frame_x,
                    StatusBarMetrics::frame_y,
                    StatusBarMetrics::frame_width + 4.0F,
                    StatusBarMetrics::frame_height,
                    {0.0F, 0.0F, 0.0F, 0.24F * opacity},
                    panel_parameters);
  draw_u_frame(renderer, layout, opacity);

  const auto text_scale =
      layout.height(StatusBarMetrics::text_size * 2.5F);
  // Aurore's text measurement API consumes the pre-geometry half scale (the
  // draw path expands it in the font geometry stage). Measuring at the draw
  // scale double-counts width and shifted right-aligned text far to the left.
  const auto measured = renderer.measure_text(time_text, text_scale * 0.5F);
  const auto text_x = layout.x(StatusBarMetrics::text_right_x) - measured.x;
  const auto text_y = layout.y(StatusBarMetrics::text_top_y);
  renderer.draw_text(time_text, text_x, text_y + layout.height(1.0F),
                     text_scale, {0.0F, 0.0F, 0.0F, 0.5F * opacity});
  renderer.draw_text(time_text, text_x, text_y, text_scale,
                     {1.0F, 1.0F, 1.0F, 0.92F * opacity});

  const auto angles = resolve_clock_hand_angles(time);
  draw_clock_face(renderer, layout, angles, 0.0F, 2.0F, 2.6F, 4.0F,
                  {0.0F, 0.0F, 0.0F, 0.55F * opacity});
  draw_clock_face(renderer, layout, angles, 0.0F, 0.0F, 5.2F, 6.2F,
                  {1.0F, 1.0F, 1.0F, 0.14F * opacity});
  draw_clock_face(renderer, layout, angles, 0.0F, 0.0F, 2.6F, 4.0F,
                  {1.0F, 1.0F, 1.0F, 0.96F * opacity});
}

bool StatusBarRenderer::render_local(
    dreamrender::gui_renderer &renderer,
    std::chrono::system_clock::time_point time, float opacity) const {
  const auto local = resolve_local_status_bar_time(time);
  if (!local.has_value()) {
    return false;
  }
  render(renderer, *local, opacity);
  return true;
}

} // namespace openxmb::xmb
