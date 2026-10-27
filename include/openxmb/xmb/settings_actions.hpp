#pragma once

#include "openxmb/xmb/catalog.hpp"
#include "openxmb/xmb/navigation_state.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace openxmb::xmb {

enum class SettingsActionKind {
  navigate_menu,
  open_dialog,
  open_wizard,
  open_option_panel,
  simulated_setting,
  simulated_action,
  landing,
  unsupported,
};

enum class SettingsActionError {
  none,
  node_not_found,
  not_settings_node,
  allocation_failure,
};

struct SettingsActionPlan {
  SettingsActionKind kind{SettingsActionKind::unsupported};
  std::string node_id;
  std::string action_id;
  std::string target_id;
  std::string persistence_key;
  std::string capability;
  SafetyClass safety{SafetyClass::safe};
  std::optional<NavigationEventKind> navigation_event;
  bool requires_confirmation{};
  bool may_update_settings_state{};

  // This dispatcher is the firewall between PS3/xmb-web catalogue semantics
  // and OpenXMB's legacy product configuration menu. A true value here would
  // mean a visible firmware label could call an unrelated OpenXMB callback.
  bool may_touch_legacy_config{};
};

struct SettingsActionResolution {
  std::optional<SettingsActionPlan> plan;
  SettingsActionError error{SettingsActionError::none};

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == SettingsActionError::none && plan.has_value();
  }
};

[[nodiscard]] bool is_settings_catalog_id(std::string_view id) noexcept;

[[nodiscard]] SettingsActionResolution
resolve_settings_action(const Catalog &catalog, std::string_view node_id) noexcept;

} // namespace openxmb::xmb
