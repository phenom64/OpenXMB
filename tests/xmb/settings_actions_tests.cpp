#include "openxmb/xmb/catalog.hpp"
#include "openxmb/xmb/settings_actions.hpp"

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

openxmb::xmb::SettingsActionPlan
plan_for(const openxmb::xmb::Catalog &catalog, std::string_view node_id) {
  const auto resolved = openxmb::xmb::resolve_settings_action(catalog, node_id);
  expect(static_cast<bool>(resolved), "settings action resolves");
  if (!resolved)
    return {};
  return *resolved.plan;
}

void expect_firewalled(const openxmb::xmb::SettingsActionPlan &plan,
                       std::string_view message) {
  expect(!plan.may_touch_legacy_config, message);
}

void test_real_catalog_routes() {
  using namespace openxmb::xmb;

  const auto loaded =
      load_catalog_file(repository_root() / "assets/xmb/catalog/en.json");
  expect(static_cast<bool>(loaded), "English catalogue loads");
  if (!loaded)
    return;
  const auto &catalog = *loaded.value;

  const auto root = plan_for(catalog, "category.settings");
  expect(root.kind == SettingsActionKind::navigate_menu &&
             root.target_id == "category.settings" &&
             root.navigation_event == NavigationEventKind::enter_menu,
         "Settings root enters the catalog-backed Settings menu");
  expect_firewalled(root, "Settings root cannot touch legacy config");

  const auto system_update =
      plan_for(catalog, "category.settings.system.update");
  expect(system_update.kind == SettingsActionKind::open_wizard &&
             system_update.target_id == "wizard.upd_check" &&
             system_update.navigation_event == NavigationEventKind::open_wizard,
         "System Update routes to the PS3 update wizard, not app self-update");
  expect_firewalled(system_update,
                    "System Update cannot call legacy updater callbacks");

  const auto video = plan_for(catalog, "category.settings.video.settings");
  expect(video.kind == SettingsActionKind::navigate_menu &&
             video.target_id == "category.settings.video.settings" &&
             video.capability == "media.video",
         "Video Settings navigates into the PS3 video settings submenu");
  expect_firewalled(video, "Video Settings cannot call Vulkan renderer config");

  const auto theme = plan_for(catalog, "category.settings.theme.settings");
  expect(theme.kind == SettingsActionKind::navigate_menu &&
             theme.target_id == "category.settings.theme.settings",
         "Theme Settings navigates to the catalog Theme Settings submenu");
  expect_firewalled(theme,
                    "Theme Settings does not reuse the legacy partial menu");

  const auto vibration = plan_for(
      catalog,
      "category.settings.accessory.settings.controller.vibration.function");
  expect(vibration.kind == SettingsActionKind::simulated_setting &&
             vibration.safety == SafetyClass::simulated &&
             vibration.may_update_settings_state &&
             vibration.persistence_key ==
                 "settings.category.settings.accessory.settings.controller."
                 "vibration.function",
         "simulated choice settings are persisted only in SettingsState");
  expect_firewalled(vibration,
                    "simulated settings cannot mutate legacy config");

  const auto format =
      plan_for(catalog, "category.settings.system.settings.format.utility");
  expect(format.kind == SettingsActionKind::open_wizard &&
             format.target_id == "wizard.fmt_method" &&
             format.safety == SafetyClass::dangerous &&
             format.requires_confirmation && !format.may_update_settings_state,
         "dangerous PS3 system actions are classified as confirmed simulated flows");
  expect_firewalled(format, "Format Utility cannot call OS destructive code");

  const auto internet = plan_for(
      catalog,
      "category.settings.network.settings.internet.connection.settings");
  expect(internet.kind == SettingsActionKind::open_wizard &&
             internet.target_id == "wizard.wiz_intro" &&
             internet.capability == "network" &&
             !internet.requires_confirmation,
         "Network Settings opens the audited xmb-web wizard route");
  expect_firewalled(internet,
                    "Network Settings cannot mutate platform networking");
}

void test_invalid_and_unsupported_routes() {
  using namespace openxmb::xmb;

  Catalog catalog;
  CatalogNode unsupported;
  unsupported.id = "category.settings.unsupported";
  unsupported.kind = NodeKind::action;
  unsupported.label_key = "unsupported.label";
  unsupported.action_id = "legacy.openxmb.renderer.config";
  catalog.nodes.emplace(unsupported.id, unsupported);

  const auto outside =
      resolve_settings_action(catalog, "category.video.display.settings");
  expect(!outside && outside.error == SettingsActionError::not_settings_node,
         "non-Settings catalog IDs are rejected");

  const auto missing =
      resolve_settings_action(catalog, "category.settings.missing");
  expect(!missing && missing.error == SettingsActionError::node_not_found,
         "missing Settings nodes are rejected");

  const auto blocked =
      resolve_settings_action(catalog, "category.settings.unsupported");
  expect(blocked && blocked.plan &&
             blocked.plan->kind == SettingsActionKind::unsupported &&
             blocked.plan->action_id == "legacy.openxmb.renderer.config" &&
             !blocked.plan->may_touch_legacy_config &&
             !blocked.plan->may_update_settings_state,
         "unknown Settings action IDs are denied by default");
}

} // namespace

int main() {
  test_real_catalog_routes();
  test_invalid_and_unsupported_routes();

  if (failures) {
    std::cerr << failures << " settings action test(s) failed\n";
    return 1;
  }
  std::cout << "settings action tests passed\n";
  return 0;
}
