#include "openxmb/xmb/settings_scene.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void expect_near(double actual, double expected, std::string_view message,
                 double tolerance = 0.0001) {
  if (std::abs(actual - expected) > tolerance) {
    std::cerr << "FAIL: " << message << " (expected " << expected << ", got "
              << actual << ")\n";
    ++failures;
  }
}

openxmb::xmb::Catalog make_catalog(std::size_t child_count = 16) {
  using namespace openxmb::xmb;
  Catalog catalog;
  catalog.locale = "en";
  CatalogNode root;
  root.id = "category.settings";
  root.kind = NodeKind::category;
  root.label_key = "settings.label";
  catalog.strings.emplace(root.label_key, "Settings");

  for (std::size_t index = 0; index < child_count; ++index) {
    CatalogNode child;
    child.id = "category.settings.item." + std::to_string(index);
    child.kind = index % 2 == 0 ? NodeKind::menu : NodeKind::setting;
    child.label_key = child.id + ".label";
    child.description_key = child.id + ".description";
    child.icon_ref = "compat.icon." + std::to_string(index);
    catalog.strings.emplace(child.label_key,
                            "Setting " + std::to_string(index));
    catalog.strings.emplace(child.description_key,
                            "Description for setting " + std::to_string(index));
    if (index == 5) {
      child.value_key = child.id + ".value";
      catalog.strings.emplace(child.value_key, "On");
      child.persistence_key = "settings.item.five";
    }
    if (index == 6) {
      child.default_selection = 1;
      child.choices.push_back(
          {child.id + ".choice.off", child.id + ".choice.off.label", "off"});
      child.choices.push_back(
          {child.id + ".choice.auto", child.id + ".choice.auto.label", "auto"});
      catalog.strings.emplace(child.id + ".choice.off.label", "Off");
      catalog.strings.emplace(child.id + ".choice.auto.label", "Auto");
    }
    if (index == 4)
      child.children.push_back("category.settings.item.4.child");
    root.children.push_back(child.id);
    catalog.nodes.emplace(child.id, std::move(child));
  }
  catalog.nodes.emplace(root.id, std::move(root));
  return catalog;
}

const openxmb::xmb::SettingsRowVisual *
find_row(const openxmb::xmb::SettingsSceneSnapshot &snapshot,
         std::size_t source_index) {
  for (std::size_t index = 0; index < snapshot.row_count; ++index) {
    if (snapshot.rows[index].source_index == source_index)
      return &snapshot.rows[index];
  }
  return nullptr;
}

void test_settled_layout_and_values() {
  using namespace openxmb::xmb;
  const auto catalog = make_catalog();
  const SettingsSceneInput input{
      .menu_id = "category.settings",
      .selected_index = 5,
      .previous_selected_index = 5,
      .can_go_back = false,
  };
  const auto sampled = sample_settings_scene(catalog, input, 20.0);
  expect(static_cast<bool>(sampled), "settled Settings scene samples");
  const auto &scene = sampled.snapshot;
  expect(scene.total_row_count == 16, "all source rows are counted");
  expect(scene.row_count == 8, "only the measured visible window is emitted");
  expect(scene.first_visible_index == 3 && scene.last_visible_index == 10,
         "visible source-index window matches the 1080p carousel");
  expect(!scene.truncated, "normal Settings window is not truncated");

  const auto *top = find_row(scene, 3);
  const auto *above = find_row(scene, 4);
  const auto *focused = find_row(scene, 5);
  const auto *below = find_row(scene, 6);
  const auto *bottom = find_row(scene, 10);
  expect(top && above && focused && below && bottom,
         "expected visible rows are present");
  if (!top || !above || !focused || !below || !bottom)
    return;
  expect_near(top->center_y, 40.0, "top visible row anchor");
  expect_near(above->center_y, 120.0, "row immediately above focus anchor");
  expect_near(focused->center_y, 515.0, "focus anchor");
  expect_near(below->center_y, 682.0,
              "first row below focus includes active padding");
  expect_near(bottom->center_y, 1002.0, "bottom visible row anchor");
  expect_near(focused->icon_extent, 78.0,
              "focused submenu icon remains at the small tier");
  expect_near(focused->label_size, 34.0, "focused label uses 34px tier");
  expect_near(above->label_size, 30.0, "inactive label uses 30px tier");
  expect_near(focused->alpha, 1.0, "focused row is fully opaque");
  expect_near(above->alpha, 0.48, "inactive row alpha is uniform");
  expect(focused->value == "On" && focused->has_value,
         "explicit localized value is exposed");
  expect(below->value == "Auto" && below->has_value,
         "default choice is the value fallback");
  expect_near(focused->value_right_x, 1840.0,
              "value is right-aligned at the measured pad");
  expect_near(focused->description_y, 547.0,
              "description starts 32px below focus");
  expect(focused->enterable, "persisted setting is activatable");
  expect(!scene.controller_hints.render_in_scene,
         "normal Settings scene intentionally has no footer hints");
  expect(scene.controller_hints.confirm_available &&
             !scene.controller_hints.back_available,
         "semantic controller actions remain available to input layers");
}

void test_focus_transition() {
  using namespace openxmb::xmb;
  const auto catalog = make_catalog();
  const SettingsSceneInput input{
      .menu_id = "category.settings",
      .selected_index = 6,
      .previous_selected_index = 5,
      .focus_transition_started_seconds = 10.0,
  };
  const auto sampled = sample_settings_scene(catalog, input, 10.1);
  expect(static_cast<bool>(sampled), "moving Settings scene samples");
  const auto &scene = sampled.snapshot;
  expect_near(scene.focus_transition_progress, 0.5,
              "focus transition is 200ms linear time");
  expect_near(scene.focus_eased_progress, 0.875,
              "focus movement uses ease-out cubic");
  expect_near(scene.description_transition_progress, 0.4,
              "description opacity has independent 250ms timing");
  const auto *old_row = find_row(scene, 5);
  const auto *new_row = find_row(scene, 6);
  expect(old_row && new_row, "old and new focus rows remain visible in motion");
  if (!old_row || !new_row)
    return;
  expect_near(old_row->center_y, 169.375,
              "old focus row interpolates to the above slot");
  expect_near(new_row->center_y, 535.875,
              "new focus row interpolates into the focus slot");
  expect(!old_row->focused && new_row->focused,
         "semantic focus changes immediately on input");
  expect_near(new_row->description_alpha, 0.28,
              "new description fades linearly at 70 percent base alpha");
  expect_near(new_row->focus_scale, 34.0 / 30.0,
              "focus text scale is represented explicitly");
}

void test_enter_exit_and_navigation_adapter() {
  using namespace openxmb::xmb;
  expect_near(SettingsSceneMetrics::activation_lock_seconds, 0.250,
              "activation lock matches the child transition duration");
  expect_near(SettingsSceneMetrics::long_press_seconds, 1.000,
              "long-press threshold remains firmware-compatible");
  const auto catalog = make_catalog();
  const NavigationRoute route{NavigationLayer::nested_menu, "category.settings",
                              5, true};
  auto entering = make_settings_scene_input(
      route, 5, 0.0, SettingsScenePresence::entering, 4.0);
  const auto entered = sample_settings_scene(catalog, entering, 4.125);
  expect(static_cast<bool>(entered), "enter transition samples");
  expect_near(entered.snapshot.presence_transition_progress, 0.5,
              "enter transition is 250ms linear time");
  expect_near(entered.snapshot.presence, 0.875,
              "enter alpha uses ease-out cubic");
  expect_near(entered.snapshot.content_x_shift, 31.25,
              "child list enters from the right over 250px");
  const auto *entered_focus = find_row(entered.snapshot, 5);
  expect(entered_focus != nullptr, "focused row is visible during enter");
  if (entered_focus) {
    expect_near(entered_focus->icon_center_x, 597.25,
                "icon rail carries the enter offset");
    expect_near(entered_focus->label_x, 714.25,
                "label rail carries the enter offset");
    expect_near(entered_focus->value_right_x, 1840.0,
                "value gutter remains fixed during enter");
  }
  expect(entered.snapshot.controller_hints.back_available,
         "nested navigation route exposes Back semantically");

  entering.presence = SettingsScenePresence::exiting;
  const auto exited = sample_settings_scene(catalog, entering, 4.125);
  expect_near(exited.snapshot.presence, 0.125,
              "exit reverses the same eased presence");
  expect_near(exited.snapshot.content_x_shift, 218.75,
              "exit moves the child list toward the right edge");
}

void test_validation_and_hidden_state() {
  using namespace openxmb::xmb;
  const auto catalog = make_catalog();
  SettingsSceneInput input{.menu_id = "category.music",
                           .selected_index = 0,
                           .previous_selected_index = 0};
  expect(sample_settings_scene(catalog, input, 0.0).error ==
             SettingsSceneError::not_a_settings_menu,
         "non-Settings nodes are rejected");
  input.menu_id = "category.settings.missing";
  expect(sample_settings_scene(catalog, input, 0.0).error ==
             SettingsSceneError::menu_not_found,
         "missing Settings node is reported");
  input.menu_id = "category.settings";
  input.selected_index = 99;
  expect(sample_settings_scene(catalog, input, 0.0).error ==
             SettingsSceneError::selection_out_of_range,
         "invalid selection is not silently clamped");
  input.selected_index = 5;
  input.previous_selected_index = 5;
  input.presence = SettingsScenePresence::hidden;
  const auto hidden = sample_settings_scene(catalog, input, 0.0);
  expect(hidden && hidden.snapshot.row_count == 0 &&
             hidden.snapshot.presence == 0.0,
         "hidden state emits no render work");

  auto malformed = make_catalog();
  malformed.nodes.at("category.settings").children[0] =
      "category.settings.missing.child";
  input.presence = SettingsScenePresence::visible;
  expect(sample_settings_scene(malformed, input, 0.0).error ==
             SettingsSceneError::unresolved_child,
         "programmatic catalogues cannot silently omit unresolved rows");
}

void test_runtime_value_override() {
  using namespace openxmb::xmb;
  const auto catalog = make_catalog();
  const SettingsSceneInput input{
      .menu_id = "category.settings",
      .selected_index = 5,
      .previous_selected_index = 5,
  };
  constexpr SettingsValueOverride overrides[]{
      {"category.settings.item.5", "Off"},
  };
  const auto sampled = sample_settings_scene(catalog, input, 0.0, overrides);
  const auto *focused = find_row(sampled.snapshot, 5);
  expect(sampled && focused && focused->value == "Off",
         "runtime persistence value overrides the catalog default");
}

} // namespace

int main() {
  test_settled_layout_and_values();
  test_focus_transition();
  test_enter_exit_and_navigation_adapter();
  test_validation_and_hidden_state();
  test_runtime_value_override();
  if (failures) {
    std::cerr << failures << " Settings scene test(s) failed\n";
    return 1;
  }
  std::cout << "Settings scene tests passed\n";
  return 0;
}
