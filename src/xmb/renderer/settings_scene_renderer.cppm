module;

#include "openxmb/xmb/settings_scene.hpp"
#include "openxmb/xmb/background.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
#include <cmath>
#include <span>
#include <string_view>

export module openxmb.xmb.settings_scene_renderer;

import auroreengine;
import openxmb.utils;
import glm;

export namespace openxmb::xmb {

struct SettingsSceneIcon {
  std::string_view semantic_id;
  const auroreengine::texture *texture{};
  const auroreengine::texture *glass_texture{};
};

struct SettingsSceneRenderOptions {
  bool glass_icons{};
  bool suppress_value_labels{};
};

class SettingsSceneRenderer {
public:
  void render(auroreengine::gui_renderer &renderer,
              const SettingsSceneSnapshot &snapshot,
              std::span<const SettingsSceneIcon> icons,
              SettingsSceneRenderOptions options = {}) const;
};

} // namespace openxmb::xmb

namespace openxmb::xmb {
namespace {

inline constexpr double settings_focused_glass_alpha = 0.90;
inline constexpr double settings_inactive_glass_alpha = 0.90;

struct ContainedLayout {
  float scale{};
  float offset_x{};
  float offset_y{};
  float framebuffer_width{};
  float framebuffer_height{};

  [[nodiscard]] float x(double logical_x) const noexcept {
    return static_cast<float>((offset_x + logical_x * scale) /
                              framebuffer_width);
  }
  [[nodiscard]] float y(double logical_y) const noexcept {
    return static_cast<float>((offset_y + logical_y * scale) /
                              framebuffer_height);
  }
  [[nodiscard]] float image_extent(double logical_extent) const noexcept {
    // Aurore images use framebuffer-height units on both axes and perform
    // aspect correction internally.
    return static_cast<float>(logical_extent * scale / framebuffer_height);
  }
  [[nodiscard]] float text_size(double logical_size) const noexcept {
    return image_extent(logical_size * 2.5);
  }
};

[[nodiscard]] ContainedLayout
make_layout(const auroreengine::gui_renderer &renderer) noexcept {
  const auto width = static_cast<float>(renderer.frame_size.width);
  const auto height = static_cast<float>(renderer.frame_size.height);
  const auto scale = static_cast<float>(std::min(
      static_cast<double>(width) / SettingsSceneMetrics::logical_width,
      static_cast<double>(height) / SettingsSceneMetrics::logical_height));
  return {
      .scale = scale,
      .offset_x = static_cast<float>(
          (width - SettingsSceneMetrics::logical_width * scale) * 0.5),
      .offset_y = static_cast<float>(
          (height - SettingsSceneMetrics::logical_height * scale) * 0.5),
      .framebuffer_width = width,
      .framebuffer_height = height,
  };
}

[[nodiscard]] const SettingsSceneIcon *
find_icon(std::span<const SettingsSceneIcon> icons,
          std::string_view semantic_id) noexcept {
  for (const auto &icon : icons) {
    if (icon.semantic_id == semantic_id)
      return &icon;
  }
  return nullptr;
}

void draw_icon(auroreengine::gui_renderer &renderer,
               const ContainedLayout &layout, const SettingsRowVisual &row,
               const SettingsSceneIcon &icon, bool glass) {
  const auto extent = layout.image_extent(row.icon_extent);
  const auto x = layout.x(row.icon_center_x - row.icon_extent * 0.5);
  const auto y = layout.y(row.center_y - row.icon_extent * 0.5);
  const auto glass_alpha = row.focused
                               ? std::min(settings_focused_glass_alpha,
                                          row.alpha * settings_focused_glass_alpha)
                               : row.alpha * settings_inactive_glass_alpha;
  const auto tint =
      glm::vec4(utils::xmb_icon_colour(openxmb::xmb::wall_clock_now()), static_cast<float>(glass ? glass_alpha : row.alpha));
  if (glass && icon.glass_texture != nullptr && icon.glass_texture->loaded)
    renderer.draw_image_glass(*icon.glass_texture, x, y, extent, extent, tint);
  else if (icon.texture != nullptr)
    renderer.draw_image_a(*icon.texture, x, y, extent, extent, tint);
}

void draw_focused_label(auroreengine::gui_renderer &renderer,
                        const ContainedLayout &layout,
                        const SettingsRowVisual &row) {
  const float pulse=0.8F+0.4F*std::sin(static_cast<float>(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count())*1.848F);
  renderer.draw_text_pixels(row.label,layout.offset_x+row.label_x*layout.scale,
      layout.offset_y+(row.center_y+3.0)*layout.scale,row.label_size*layout.scale,
      {1,1,1,static_cast<float>(row.alpha)},0.72F,pulse);

}

void draw_wrapped_description(auroreengine::gui_renderer &renderer,
                              const ContainedLayout &layout,
                              const SettingsRowVisual &row) {
  if (!row.has_description || row.description_alpha <= 0.001 ||
      row.description_max_width <= 0.0)
    return;

  const auto size = layout.text_size(row.description_size);
  const auto max_width = static_cast<float>(
      row.description_max_width * layout.scale / layout.framebuffer_width);
  auto cursor = std::size_t{};
  auto line = std::size_t{};
  while (cursor < row.description.size() &&
         line < SettingsSceneMetrics::max_description_lines) {
    while (cursor < row.description.size() &&
           (row.description[cursor] == ' ' || row.description[cursor] == '\n'))
      ++cursor;
    if (cursor >= row.description.size())
      break;

    const auto line_start = cursor;
    auto accepted_end = cursor;
    while (cursor < row.description.size()) {
      const auto word_start = cursor;
      while (cursor < row.description.size() &&
             row.description[cursor] != ' ' && row.description[cursor] != '\n')
        ++cursor;
      const auto candidate_end = cursor;
      const auto candidate =
          row.description.substr(line_start, candidate_end - line_start);
      // Aurore's measurement consumes the pre-geometry half scale.
      if (accepted_end != line_start &&
          renderer.measure_text(candidate, size * 0.5F).x > max_width) {
        cursor = word_start;
        break;
      }
      accepted_end = candidate_end;
      if (cursor < row.description.size() && row.description[cursor] == '\n') {
        ++cursor;
        break;
      }
      while (cursor < row.description.size() && row.description[cursor] == ' ')
        ++cursor;
    }
    if (accepted_end == line_start)
      accepted_end = cursor;
    const auto text =
        row.description.substr(line_start, accepted_end - line_start);
    renderer.draw_text_pixels(text,layout.offset_x+row.description_x*layout.scale,
        layout.offset_y+(row.description_y+static_cast<double>(line)*row.description_line_height)*layout.scale,
        22.0F*layout.scale,{0.92F,0.92F,0.92F,static_cast<float>(row.description_alpha)},0.65F);
    ++line;
  }
}

} // namespace

void SettingsSceneRenderer::render(auroreengine::gui_renderer &renderer,
                                   const SettingsSceneSnapshot &snapshot,
                                   std::span<const SettingsSceneIcon> icons,
                                   SettingsSceneRenderOptions options) const {
  const auto layout = make_layout(renderer);
  for (std::size_t index = 0; index < snapshot.row_count; ++index) {
    const auto &row = snapshot.rows[index];
    if (row.alpha <= 0.001)
      continue;

    if (const auto *icon = find_icon(icons, row.icon_ref))
      draw_icon(renderer, layout, row, *icon, options.glass_icons);

    if (row.focused) {
      draw_focused_label(renderer, layout, row);
      draw_wrapped_description(renderer, layout, row);
    } else {
      const auto value = 235.0F / 255.0F;
      renderer.draw_text_pixels(row.label,layout.offset_x+row.label_x*layout.scale,
          layout.offset_y+(row.center_y+11.0)*layout.scale,row.label_size*layout.scale,
          {value,value,value,static_cast<float>(row.alpha)},0.7F);
    }

    if (!options.suppress_value_labels && row.has_value &&
        row.value_alpha > 0.001) {
      const auto size = layout.text_size(row.value_size);
      const auto measured = renderer.measure_text(row.value, size * 0.5F);
      const auto right = layout.x(row.value_right_x);
      const auto color =
          row.focused
              ? glm::vec3(245.0F / 255.0F, 240.0F / 255.0F, 250.0F / 255.0F)
              : glm::vec3(235.0F / 255.0F);
      renderer.draw_text(
          row.value, right - measured.x, layout.y(row.center_y), size,
          glm::vec4(color, static_cast<float>(row.value_alpha)), false, true);
    }
  }
}

} // namespace openxmb::xmb
