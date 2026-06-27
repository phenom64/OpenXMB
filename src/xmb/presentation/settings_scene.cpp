#include "openxmb/xmb/settings_scene.hpp"

#include <algorithm>
#include <cmath>

namespace openxmb::xmb {
namespace {

[[nodiscard]] constexpr double saturate(double value) noexcept {
  return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] constexpr double ease_out_cubic(double value) noexcept {
  const auto t = saturate(value);
  const auto inverse = 1.0 - t;
  return 1.0 - inverse * inverse * inverse;
}

[[nodiscard]] constexpr double progress(double now, double started,
                                        double duration) noexcept {
  if (!std::isfinite(now) || !std::isfinite(started) || duration <= 0.0)
    return 1.0;
  return saturate((now - started) / duration);
}

[[nodiscard]] constexpr double slot_y(std::size_t index,
                                      std::size_t selected) noexcept {
  if (index == selected)
    return SettingsSceneMetrics::focus_y;
  if (index < selected) {
    return SettingsSceneMetrics::item_above_base_y -
           static_cast<double>(selected - index - 1) *
               SettingsSceneMetrics::item_pitch;
  }
  return SettingsSceneMetrics::focus_y +
         SettingsSceneMetrics::selected_to_next_padding +
         static_cast<double>(index - selected) *
             SettingsSceneMetrics::item_pitch;
}

[[nodiscard]] constexpr bool is_settings_id(std::string_view id) noexcept {
  constexpr std::string_view root = "category.settings";
  return id == root || (id.size() > root.size() && id.starts_with(root) &&
                        id[root.size()] == '.');
}

[[nodiscard]] const CatalogNode *find_node(const Catalog &catalog,
                                           std::string_view id) noexcept {
  const auto found = catalog.nodes.find(id);
  return found == catalog.nodes.end() ? nullptr : &found->second;
}

[[nodiscard]] std::string_view text(const Catalog &catalog,
                                    std::string_view key) noexcept {
  if (key.empty())
    return {};
  const auto found = catalog.strings.find(key);
  return found == catalog.strings.end() ? std::string_view{}
                                        : std::string_view(found->second);
}

[[nodiscard]] std::string_view
resolve_value(const Catalog &catalog, const CatalogNode &node,
              std::span<const SettingsValueOverride> overrides) noexcept {
  for (const auto &override_value : overrides) {
    if (override_value.node_id == node.id)
      return override_value.localized_value;
  }
  if (const auto direct = text(catalog, node.value_key); !direct.empty())
    return direct;
  if (node.default_selection < node.choices.size())
    return text(catalog, node.choices[node.default_selection].label_key);
  return {};
}

[[nodiscard]] constexpr bool enterable(const CatalogNode &node) noexcept {
  return !node.children.empty() || !node.action_id.empty() ||
         !node.choices.empty() || !node.persistence_key.empty();
}

[[nodiscard]] constexpr double edge_alpha(double y) noexcept {
  auto alpha = 1.0;
  if (y <
      SettingsSceneMetrics::clip_top + SettingsSceneMetrics::top_fade_extent) {
    alpha *= std::max(0.0, (y - SettingsSceneMetrics::clip_top) /
                               SettingsSceneMetrics::top_fade_extent);
  }
  if (y > SettingsSceneMetrics::clip_bottom -
              SettingsSceneMetrics::bottom_fade_extent) {
    alpha *= std::max(0.0, (SettingsSceneMetrics::clip_bottom - y) /
                               SettingsSceneMetrics::bottom_fade_extent);
  }
  return alpha;
}

} // namespace

SettingsSceneInput make_settings_scene_input(
    const NavigationRoute &route, std::size_t previous_selection,
    double focus_transition_started_seconds, SettingsScenePresence presence,
    double presence_transition_started_seconds) noexcept {
  return {
      .menu_id = route.id,
      .selected_index = route.selection,
      .previous_selected_index = previous_selection,
      .focus_transition_started_seconds = focus_transition_started_seconds,
      .presence = presence,
      .presence_transition_started_seconds =
          presence_transition_started_seconds,
      .can_go_back = route.layer == NavigationLayer::nested_menu,
  };
}

SettingsSceneSample sample_settings_scene(
    const Catalog &catalog, const SettingsSceneInput &input, double now_seconds,
    std::span<const SettingsValueOverride> value_overrides) noexcept {
  SettingsSceneSample result{};
  if (!is_settings_id(input.menu_id)) {
    result.error = SettingsSceneError::not_a_settings_menu;
    return result;
  }

  const auto *menu = find_node(catalog, input.menu_id);
  if (!menu) {
    result.error = SettingsSceneError::menu_not_found;
    return result;
  }
  if (menu->children.empty()) {
    result.error = SettingsSceneError::empty_menu;
    return result;
  }
  if (input.selected_index >= menu->children.size() ||
      input.previous_selected_index >= menu->children.size()) {
    result.error = SettingsSceneError::selection_out_of_range;
    return result;
  }
  for (const auto &child_id : menu->children) {
    if (!find_node(catalog, child_id)) {
      result.error = SettingsSceneError::unresolved_child;
      return result;
    }
  }

  auto &snapshot = result.snapshot;
  snapshot.total_row_count = menu->children.size();
  snapshot.selected_index = input.selected_index;
  snapshot.focus_transition_progress =
      input.selected_index == input.previous_selected_index
          ? 1.0
          : progress(now_seconds, input.focus_transition_started_seconds,
                     SettingsSceneMetrics::focus_transition_seconds);
  snapshot.focus_eased_progress =
      ease_out_cubic(snapshot.focus_transition_progress);
  snapshot.description_transition_progress =
      input.selected_index == input.previous_selected_index
          ? 1.0
          : progress(now_seconds, input.focus_transition_started_seconds,
                     SettingsSceneMetrics::description_transition_seconds);

  switch (input.presence) {
  case SettingsScenePresence::visible:
    snapshot.presence_transition_progress = 1.0;
    snapshot.presence = 1.0;
    break;
  case SettingsScenePresence::hidden:
    snapshot.presence_transition_progress = 1.0;
    snapshot.presence = 0.0;
    break;
  case SettingsScenePresence::entering:
  case SettingsScenePresence::exiting: {
    snapshot.presence_transition_progress =
        progress(now_seconds, input.presence_transition_started_seconds,
                 SettingsSceneMetrics::presence_transition_seconds);
    const auto eased = ease_out_cubic(snapshot.presence_transition_progress);
    snapshot.presence =
        input.presence == SettingsScenePresence::entering ? eased : 1.0 - eased;
    break;
  }
  }
  snapshot.content_x_shift =
      SettingsSceneMetrics::child_slide_distance * (1.0 - snapshot.presence);

  if (snapshot.presence <= 0.001)
    return result;

  const auto label_x = SettingsSceneMetrics::label_x + snapshot.content_x_shift;
  const auto icon_x =
      SettingsSceneMetrics::icon_center_x + snapshot.content_x_shift;

  for (std::size_t index = 0; index < menu->children.size(); ++index) {
    const auto old_y = slot_y(index, input.previous_selected_index);
    const auto new_y = slot_y(index, input.selected_index);
    const auto y = old_y + (new_y - old_y) * snapshot.focus_eased_progress;
    if (y < SettingsSceneMetrics::clip_top -
                SettingsSceneMetrics::cull_overscan ||
        y > SettingsSceneMetrics::clip_bottom +
                SettingsSceneMetrics::cull_overscan) {
      continue;
    }

    const auto clipped_alpha = edge_alpha(y);
    if (clipped_alpha <= 0.01)
      continue;

    const auto *node = find_node(catalog, menu->children[index]);

    if (snapshot.row_count >= snapshot.rows.size()) {
      snapshot.truncated = true;
      continue;
    }

    const auto focused = index == input.selected_index;
    const auto base_alpha = focused ? SettingsSceneMetrics::focus_alpha
                                    : SettingsSceneMetrics::inactive_alpha;
    const auto alpha = base_alpha * clipped_alpha * snapshot.presence;
    const auto value = resolve_value(catalog, *node, value_overrides);
    auto &row = snapshot.rows[snapshot.row_count++];
    row = {
        .source_index = index,
        .id = node->id,
        .label = text(catalog, node->label_key),
        .description = text(catalog, node->description_key),
        .value = value,
        .icon_ref = node->icon_ref,
        .icon_center_x = icon_x,
        .center_y = y,
        .icon_extent = SettingsSceneMetrics::icon_extent,
        .label_x = label_x,
        .value_right_x = SettingsSceneMetrics::value_right_x,
        .label_size = focused ? SettingsSceneMetrics::active_label_size
                              : SettingsSceneMetrics::inactive_label_size,
        .value_size = SettingsSceneMetrics::inactive_label_size,
        .description_x = label_x,
        .description_y = y + SettingsSceneMetrics::description_offset_y,
        .description_size = SettingsSceneMetrics::description_size,
        .description_line_height =
            SettingsSceneMetrics::description_line_height,
        .description_max_width =
            std::max(0.0, SettingsSceneMetrics::logical_width - label_x -
                              SettingsSceneMetrics::description_right_margin),
        .alpha = alpha,
        .value_alpha = alpha,
        .description_alpha = focused
                                 ? 0.7 * snapshot.presence *
                                       snapshot.description_transition_progress
                                 : 0.0,
        .focus_scale = focused ? SettingsSceneMetrics::active_label_size /
                                     SettingsSceneMetrics::inactive_label_size
                               : 1.0,
        .focused = focused,
        .enterable = enterable(*node),
        .has_value = !value.empty(),
        .has_description = !text(catalog, node->description_key).empty(),
    };

    if (snapshot.first_visible_index ==
        std::numeric_limits<std::size_t>::max()) {
      snapshot.first_visible_index = index;
    }
    snapshot.last_visible_index = index;
  }

  const auto *selected =
      find_node(catalog, menu->children[input.selected_index]);
  snapshot.controller_hints = {
      .render_in_scene = false,
      .confirm_available = selected && enterable(*selected),
      .back_available = input.can_go_back,
  };
  return result;
}

} // namespace openxmb::xmb
