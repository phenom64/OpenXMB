#include "openxmb/xmb/settings_actions.hpp"

#include <new>

namespace openxmb::xmb {
namespace {

[[nodiscard]] constexpr bool starts_with(std::string_view value,
                                         std::string_view prefix) noexcept {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] bool is_simulated_setting(const CatalogNode &node) noexcept {
  return !node.persistence_key.empty() || !node.choices.empty();
}

[[nodiscard]] SettingsActionKind classify(const CatalogNode &node) noexcept {
  const auto action = std::string_view{node.action_id};
  if (!node.children.empty() || action == "navigate.children" ||
      action == "navigate.category") {
    return SettingsActionKind::navigate_menu;
  }
  if (starts_with(action, "dialog."))
    return SettingsActionKind::open_dialog;
  if (starts_with(action, "wizard."))
    return SettingsActionKind::open_wizard;
  if (starts_with(action, "control."))
    return SettingsActionKind::open_option_panel;
  if (starts_with(action, "landing."))
    return SettingsActionKind::landing;
  if (starts_with(action, "simulate."))
    return is_simulated_setting(node) ? SettingsActionKind::simulated_setting
                                      : SettingsActionKind::simulated_action;
  if (action.empty() && is_simulated_setting(node))
    return SettingsActionKind::simulated_setting;
  return SettingsActionKind::unsupported;
}

[[nodiscard]] std::optional<NavigationEventKind>
navigation_event_for(SettingsActionKind kind) noexcept {
  switch (kind) {
  case SettingsActionKind::navigate_menu:
    return NavigationEventKind::enter_menu;
  case SettingsActionKind::open_dialog:
    return NavigationEventKind::open_modal;
  case SettingsActionKind::open_wizard:
    return NavigationEventKind::open_wizard;
  case SettingsActionKind::open_option_panel:
    return NavigationEventKind::open_option_panel;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] std::string_view target_for(const CatalogNode &node,
                                          SettingsActionKind kind) noexcept {
  switch (kind) {
  case SettingsActionKind::navigate_menu:
  case SettingsActionKind::simulated_setting:
  case SettingsActionKind::simulated_action:
  case SettingsActionKind::unsupported:
    return node.id;
  case SettingsActionKind::open_dialog:
  case SettingsActionKind::open_wizard:
  case SettingsActionKind::open_option_panel:
  case SettingsActionKind::landing:
    return node.action_id;
  }
  return node.id;
}

} // namespace

bool is_settings_catalog_id(std::string_view id) noexcept {
  constexpr std::string_view root = "category.settings";
  return id == root || (id.size() > root.size() && starts_with(id, root) &&
                        id[root.size()] == '.');
}

SettingsActionResolution
resolve_settings_action(const Catalog &catalog, std::string_view node_id) noexcept {
  if (!is_settings_catalog_id(node_id)) {
    return {std::nullopt, SettingsActionError::not_settings_node};
  }

  const auto *node = catalog.find_node(node_id);
  if (!node) {
    return {std::nullopt, SettingsActionError::node_not_found};
  }

  try {
    SettingsActionPlan plan;
    plan.kind = classify(*node);
    plan.node_id = node->id;
    plan.action_id = node->action_id;
    plan.target_id = std::string{target_for(*node, plan.kind)};
    plan.persistence_key = node->persistence_key;
    plan.capability = node->capability;
    plan.safety = node->safety;
    plan.navigation_event = navigation_event_for(plan.kind);
    plan.requires_confirmation = node->safety == SafetyClass::dangerous;
    plan.may_update_settings_state =
        plan.kind == SettingsActionKind::simulated_setting &&
        !plan.requires_confirmation;
    plan.may_touch_legacy_config = false;
    return {std::move(plan), SettingsActionError::none};
  } catch (const std::bad_alloc &) {
    return {std::nullopt, SettingsActionError::allocation_failure};
  } catch (...) {
    return {std::nullopt, SettingsActionError::allocation_failure};
  }
}

} // namespace openxmb::xmb
