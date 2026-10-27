#include "openxmb/xmb/catalog.hpp"
#include "openxmb/xmb/settings_controller.hpp"

#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::filesystem::path repository_root() {
  auto candidate = std::filesystem::path(__FILE__).parent_path();
  while (!candidate.empty()) {
    if (std::filesystem::exists(candidate / "assets/xmb/catalog/en.json"))
      return candidate;
    const auto parent = candidate.parent_path();
    if (parent == candidate)
      break;
    candidate = parent;
  }
  return std::filesystem::current_path();
}

openxmb::xmb::Catalog load_real_catalog() {
  const auto loaded = openxmb::xmb::load_catalog_file(
      repository_root() / "assets/xmb/catalog/en.json");
  expect(static_cast<bool>(loaded), "English catalogue loads");
  return loaded ? std::move(*loaded.value) : openxmb::xmb::Catalog{};
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

void test_create_and_sample_scene() {
  using namespace openxmb::xmb;
  const auto catalog = load_real_catalog();
  auto created = SettingsCatalogController::create(catalog, 10.0);
  expect(static_cast<bool>(created), "Settings controller creates");
  if (!created)
    return;

  auto &controller = *created.controller;
  expect(controller.route().layer == NavigationLayer::nested_menu &&
             controller.route().id == "category.settings" &&
             controller.route().selection == 5,
         "controller starts on the xmb-web Settings default selection");
  expect(controller.navigation().active_category == "category.settings",
         "controller records Settings as the active category");

  const auto scene = controller.sample_scene(10.5);
  expect(static_cast<bool>(scene), "controller samples Settings scene");
  expect(scene.snapshot.selected_index == 5,
         "scene uses controller selection");
  const auto *focused = find_row(scene.snapshot, 5);
  expect(focused && focused->label == "System Settings" && focused->focused,
         "System Settings is the focused initial row");
}

void test_activation_routes_and_back() {
  using namespace openxmb::xmb;
  const auto catalog = load_real_catalog();
  auto created = SettingsCatalogController::create(catalog, 0.0);
  expect(static_cast<bool>(created), "Settings controller creates");
  if (!created)
    return;
  auto &controller = *created.controller;

  auto select_update = controller.set_selection(0, 1.0);
  expect(select_update && select_update.changed,
         "selection changes to System Update");
  auto update = controller.activate(1.1);
  expect(update && update.action &&
             update.action->kind == SettingsActionKind::open_wizard &&
             update.action->target_id == "wizard.upd_check" &&
             controller.route().layer == NavigationLayer::wizard &&
             controller.route().id == "wizard.upd_check",
         "System Update activation opens the catalog wizard route");
  expect(!update.action->may_touch_legacy_config,
         "System Update still cannot call legacy update code");

  auto back_to_settings = controller.back(1.2);
  expect(back_to_settings && controller.route().id == "category.settings" &&
             controller.route().selection == 0,
         "back returns from the wizard to the selected Settings row");

  auto select_video = controller.set_selection(2, 2.0);
  expect(select_video && select_video.changed, "selection changes to Video");
  auto video = controller.activate(2.1);
  expect(video && video.action &&
             video.action->kind == SettingsActionKind::navigate_menu &&
             video.action->target_id == "category.settings.video.settings" &&
             controller.route().layer == NavigationLayer::nested_menu &&
             controller.route().id == "category.settings.video.settings" &&
             controller.route().selection == 0,
         "Video Settings activation enters the catalog submenu");
  const auto video_scene = controller.sample_scene(2.5);
  expect(video_scene && video_scene.snapshot.total_row_count == 1,
         "Video Settings submenu samples from catalog children");

  auto back_to_root_settings = controller.back(3.0);
  expect(back_to_root_settings && controller.route().id == "category.settings",
         "back returns from Video Settings submenu");
}

void test_simulated_and_dangerous_settings() {
  using namespace openxmb::xmb;
  const auto catalog = load_real_catalog();
  auto created = SettingsCatalogController::create(catalog, 0.0);
  expect(static_cast<bool>(created), "Settings controller creates");
  if (!created)
    return;
  auto &controller = *created.controller;

  expect(static_cast<bool>(controller.set_selection(9, 1.0)),
         "select Accessory Settings");
  const auto accessory = controller.activate(1.1);
  expect(accessory && accessory.action &&
             accessory.action->target_id == "category.settings.accessory.settings",
         "Accessory Settings opens as a nested catalog menu");
  expect(static_cast<bool>(controller.set_selection(2, 1.2)),
         "select Controller Vibration Function");
  const auto vibration = controller.activate(1.3);
  expect(vibration && vibration.action &&
             vibration.action->kind == SettingsActionKind::simulated_setting &&
             vibration.action->may_update_settings_state &&
             !vibration.action->may_touch_legacy_config &&
             controller.route().id == "category.settings.accessory.settings",
         "simulated setting activation returns a SettingsState-only plan");

  expect(static_cast<bool>(controller.back(2.0)),
         "return to Settings root");
  expect(static_cast<bool>(controller.set_selection(5, 2.1)),
         "select System Settings");
  const auto system = controller.activate(2.2);
  expect(system && system.action &&
             system.action->target_id == "category.settings.system.settings",
         "System Settings opens as a nested catalog menu");
  expect(static_cast<bool>(controller.set_selection(18, 2.3)),
         "select Format Utility");
  const auto format = controller.activate(2.4);
  expect(format && format.action &&
             format.action->kind == SettingsActionKind::open_wizard &&
             format.action->target_id == "wizard.fmt_method" &&
             format.action->requires_confirmation &&
             !format.action->may_update_settings_state &&
             !format.action->may_touch_legacy_config &&
             controller.route().layer == NavigationLayer::wizard,
         "dangerous Format Utility opens only a confirmed simulated wizard");
}

void test_validation_paths() {
  using namespace openxmb::xmb;
  Catalog empty;
  auto missing = SettingsCatalogController::create(empty);
  expect(!missing && missing.error == SettingsControllerError::missing_root,
         "controller creation requires the Settings catalog root");

  Catalog malformed;
  CatalogNode root;
  root.id = "category.settings";
  root.action_id = "navigate.category";
  malformed.nodes.emplace(root.id, std::move(root));
  auto invalid = SettingsCatalogController::create(malformed);
  expect(!invalid && invalid.error == SettingsControllerError::invalid_menu,
         "controller creation rejects an empty Settings root");

  const auto catalog = load_real_catalog();
  auto created = SettingsCatalogController::create(catalog);
  expect(static_cast<bool>(created), "Settings controller creates");
  if (!created)
    return;
  auto bad_selection = created.controller->set_selection(999, 0.0);
  expect(!bad_selection &&
             bad_selection.error == SettingsControllerError::selection_out_of_range,
         "controller rejects out-of-range selection");
}

} // namespace

int main() {
  test_create_and_sample_scene();
  test_activation_routes_and_back();
  test_simulated_and_dangerous_settings();
  test_validation_paths();

  if (failures) {
    std::cerr << failures << " settings controller test(s) failed\n";
    return 1;
  }
  std::cout << "settings controller tests passed\n";
  return 0;
}
