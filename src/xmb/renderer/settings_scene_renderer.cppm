module;

#include "openxmb/xmb/settings_scene.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>

export module openxmb.xmb.settings_scene_renderer;

import dreamrender;
import glm;

export namespace openxmb::xmb {

struct SettingsSceneIcon {
  std::string_view semantic_id;
  const dreamrender::texture *texture{};
  const dreamrender::texture *glass_texture{};
};

struct SettingsSceneRenderOptions {
  bool glass_icons{};
};

class SettingsSceneRenderer {
public:
  void render(dreamrender::gui_renderer &renderer,
              const SettingsSceneSnapshot &snapshot,
              std::span<const SettingsSceneIcon> icons,
              SettingsSceneRenderOptions options = {}) const;
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
make_layout(const dreamrender::gui_renderer &renderer) noexcept {
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

void draw_icon(dreamrender::gui_renderer &renderer,
               const ContainedLayout &layout, const SettingsRowVisual &row,
               const SettingsSceneIcon &icon, bool glass) {
  const auto extent = layout.image_extent(row.icon_extent);
  const auto x = layout.x(row.icon_center_x - row.icon_extent * 0.5);
  const auto y = layout.y(row.center_y - row.icon_extent * 0.5);
  const auto glass_alpha = row.focused
                               ? std::min(1.0, row.alpha * 1.08)
                               : row.alpha;
  const auto tint =
      glm::vec4(1.0F, 1.0F, 1.0F, static_cast<float>(glass ? glass_alpha : row.alpha));
  if (glass && icon.glass_texture != nullptr && icon.glass_texture->loaded)
    renderer.draw_image_glass(*icon.glass_texture, x, y, extent, extent, tint);
  else if (icon.texture != nullptr)
    renderer.draw_image_a(*icon.texture, x, y, extent, extent, tint);
}

void draw_focused_label(dreamrender::gui_renderer &renderer,
                        const ContainedLayout &layout,
                        const SettingsRowVisual &row) {
  const auto x = layout.x(row.label_x);
  const auto y = layout.y(row.center_y - 1.0);
  const auto size = layout.text_size(row.label_size);
  const auto alpha = static_cast<float>(row.alpha);
  // A bounded four-direction halo approximates xmb-web's two Canvas blur
  // passes without allocating temporary geometry or textures.
  constexpr std::array<std::array<double, 2>, 8> offsets{{
      {{-4.0, 0.0}},
      {{4.0, 0.0}},
      {{0.0, -4.0}},
      {{0.0, 4.0}},
      {{-2.0, -2.0}},
      {{2.0, -2.0}},
      {{-2.0, 2.0}},
      {{2.0, 2.0}},
  }};
  for (const auto &offset : offsets) {
    renderer.draw_text(row.label, layout.x(row.label_x + offset[0]),
                       layout.y(row.center_y - 1.0 + offset[1]), size,
                       glm::vec4(1.0F, 1.0F, 1.0F, alpha * 0.075F), false,
                       true);
  }
  renderer.draw_text(row.label, x, y, size, glm::vec4(1.0F, 1.0F, 1.0F, alpha),
                     false, true);
}

void draw_wrapped_description(dreamrender::gui_renderer &renderer,
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
    renderer.draw_text(
        text, layout.x(row.description_x),
        layout.y(row.description_y +
                 static_cast<double>(line) * row.description_line_height),
        size,
        glm::vec4(235.0F / 255.0F, 235.0F / 255.0F, 235.0F / 255.0F,
                  static_cast<float>(row.description_alpha)),
        false, false);
    ++line;
  }
}

} // namespace

void SettingsSceneRenderer::render(dreamrender::gui_renderer &renderer,
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
      renderer.draw_text(
          row.label, layout.x(row.label_x), layout.y(row.center_y),
          layout.text_size(row.label_size),
          glm::vec4(value, value, value, static_cast<float>(row.alpha)), false,
          true);
    }

    if (row.has_value && row.value_alpha > 0.001) {
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
