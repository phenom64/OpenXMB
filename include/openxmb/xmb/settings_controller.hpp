#pragma once

#include "openxmb/xmb/catalog.hpp"
#include "openxmb/xmb/navigation_state.hpp"
#include "openxmb/xmb/settings_actions.hpp"
#include "openxmb/xmb/settings_scene.hpp"

#include <cstddef>
#include <optional>
#include <span>

namespace openxmb::xmb {

enum class SettingsControllerError {
  none,
  missing_root,
  invalid_menu,
  selection_out_of_range,
  action_resolution_failed,
  navigation_rejected,
  allocation_failure,
};

struct SettingsControllerStep {
  std::optional<SettingsActionPlan> action;
  SettingsControllerError error{SettingsControllerError::none};
  bool changed{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == SettingsControllerError::none;
  }
};

struct SettingsControllerCreateResult;

class SettingsCatalogController final {
public:
  [[nodiscard]] static SettingsControllerCreateResult
  create(const Catalog &catalog, double now_seconds = 0.0) noexcept;

  [[nodiscard]] const NavigationState &navigation() const noexcept {
    return navigation_;
  }
  [[nodiscard]] const NavigationRoute &route() const noexcept {
    return navigation_.top();
  }
  [[nodiscard]] std::size_t previous_selection() const noexcept {
    return previous_selection_;
  }
  [[nodiscard]] double focus_transition_started_seconds() const noexcept {
    return focus_transition_started_seconds_;
  }

  [[nodiscard]] SettingsSceneSample sample_scene(
      double now_seconds,
      std::span<const SettingsValueOverride> value_overrides = {}) const
      noexcept;

  [[nodiscard]] SettingsControllerStep set_selection(std::size_t selection,
                                                     double now_seconds) noexcept;
  [[nodiscard]] SettingsControllerStep move_selection(int delta,
                                                      double now_seconds) noexcept;
  [[nodiscard]] SettingsControllerStep activate(double now_seconds) noexcept;
  [[nodiscard]] SettingsControllerStep back(double now_seconds) noexcept;

private:
  SettingsCatalogController(const Catalog &catalog, NavigationState navigation,
                            std::size_t previous_selection,
                            double focus_transition_started_seconds) noexcept;

  [[nodiscard]] const CatalogNode *active_menu() const noexcept;

  const Catalog *catalog_{};
  NavigationState navigation_;
  std::size_t previous_selection_{};
  double focus_transition_started_seconds_{};
};

struct SettingsControllerCreateResult {
  std::optional<SettingsCatalogController> controller;
  SettingsControllerError error{SettingsControllerError::none};

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == SettingsControllerError::none && controller.has_value();
  }
};

} // namespace openxmb::xmb
