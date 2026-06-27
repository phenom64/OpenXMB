#pragma once

#include "openxmb/xmb/catalog.hpp"
#include "openxmb/xmb/navigation_state.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <string_view>

namespace openxmb::xmb {

// Measurements are expressed in xmb-web's native 1920x1080 design space. The
// renderer contains that design rectangle inside the framebuffer without
// changing its aspect ratio.
struct SettingsSceneMetrics {
  static constexpr double logical_width = 1920.0;
  static constexpr double logical_height = 1080.0;
  static constexpr double icon_center_x = 566.0;
  static constexpr double label_x = 683.0;
  static constexpr double value_right_x = 1840.0;
  static constexpr double focus_y = 515.0;
  static constexpr double item_above_base_y = 120.0;
  static constexpr double item_pitch = 80.0;
  static constexpr double selected_to_next_padding = 87.0;
  static constexpr double icon_extent = 78.0;
  static constexpr double inactive_label_size = 30.0;
  static constexpr double active_label_size = 34.0;
  static constexpr double description_size = 18.0;
  static constexpr double description_offset_y = 32.0;
  static constexpr double description_line_height = 22.0;
  static constexpr double description_right_margin = 200.0;
  static constexpr double inactive_alpha = 0.48;
  static constexpr double focus_alpha = 1.0;
  static constexpr double focus_transition_seconds = 0.200;
  static constexpr double description_transition_seconds = 0.250;
  static constexpr double presence_transition_seconds = 0.250;
  static constexpr double activation_lock_seconds = 0.250;
  static constexpr double long_press_seconds = 1.000;
  static constexpr double child_slide_distance = 250.0;
  static constexpr double clip_top = 0.0;
  static constexpr double clip_bottom = 1080.0;
  static constexpr double top_fade_extent = 40.0;
  static constexpr double bottom_fade_extent = 20.0;
  static constexpr double cull_overscan = 80.0;
  static constexpr std::size_t max_visible_rows = 32;
  static constexpr std::size_t max_description_lines = 3;
};

enum class SettingsScenePresence { visible, entering, exiting, hidden };

// This is a borrowed, allocation-free view of the navigation state. menu_id
// remains valid only while the NavigationRoute (or its id storage) remains
// alive.
struct SettingsSceneInput {
  std::string_view menu_id;
  std::size_t selected_index{};
  std::size_t previous_selected_index{};
  double focus_transition_started_seconds{};
  SettingsScenePresence presence{SettingsScenePresence::visible};
  double presence_transition_started_seconds{};
  bool can_go_back{};
};

struct SettingsRowVisual {
  std::size_t source_index{};
  std::string_view id;
  std::string_view label;
  std::string_view description;
  std::string_view value;
  std::string_view icon_ref;
  double icon_center_x{};
  double center_y{};
  double icon_extent{};
  double label_x{};
  double value_right_x{};
  double label_size{};
  double value_size{};
  double description_x{};
  double description_y{};
  double description_size{};
  double description_line_height{};
  double description_max_width{};
  double alpha{};
  double value_alpha{};
  double description_alpha{};
  double focus_scale{};
  bool focused{};
  bool enterable{};
  bool has_value{};
  bool has_description{};
};

struct SettingsControllerHintPolicy {
  // xmb-web does not draw a normal-XMB footer hint bar. Availability remains
  // in the snapshot for input/accessibility layers without adding chrome.
  bool render_in_scene{};
  bool confirm_available{};
  bool back_available{};
};

struct SettingsSceneSnapshot {
  std::array<SettingsRowVisual, SettingsSceneMetrics::max_visible_rows> rows{};
  std::size_t row_count{};
  std::size_t total_row_count{};
  std::size_t first_visible_index{std::numeric_limits<std::size_t>::max()};
  std::size_t last_visible_index{std::numeric_limits<std::size_t>::max()};
  std::size_t selected_index{};
  double focus_transition_progress{1.0};
  double focus_eased_progress{1.0};
  double description_transition_progress{1.0};
  double presence_transition_progress{1.0};
  double presence{};
  double content_x_shift{};
  bool truncated{};
  SettingsControllerHintPolicy controller_hints{};
};

enum class SettingsSceneError {
  none,
  menu_not_found,
  not_a_settings_menu,
  empty_menu,
  selection_out_of_range,
  unresolved_child,
};

struct SettingsSceneSample {
  SettingsSceneSnapshot snapshot{};
  SettingsSceneError error{SettingsSceneError::none};

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == SettingsSceneError::none;
  }
};

// Runtime settings/persistence layers may override Catalog defaults without
// copying localized strings into the scene. Both fields are borrowed.
struct SettingsValueOverride {
  std::string_view node_id;
  std::string_view localized_value;
};

[[nodiscard]] SettingsSceneInput make_settings_scene_input(
    const NavigationRoute &route, std::size_t previous_selection,
    double focus_transition_started_seconds,
    SettingsScenePresence presence = SettingsScenePresence::visible,
    double presence_transition_started_seconds = 0.0) noexcept;

[[nodiscard]] SettingsSceneSample sample_settings_scene(
    const Catalog &catalog, const SettingsSceneInput &input, double now_seconds,
    std::span<const SettingsValueOverride> value_overrides = {}) noexcept;

} // namespace openxmb::xmb
