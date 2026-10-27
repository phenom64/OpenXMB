#include "openxmb/xmb/settings_controller.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace openxmb::xmb {
namespace {

[[nodiscard]] SettingsControllerError
navigation_error_to_controller(NavigationError error) noexcept {
  return error == NavigationError::allocation_failure
             ? SettingsControllerError::allocation_failure
             : SettingsControllerError::navigation_rejected;
}

[[nodiscard]] SettingsControllerStep apply_navigation(
    NavigationState &state, const NavigationEvent &event) noexcept {
  const auto result = transition(state, event);
  if (!result || !result.state) {
    return {std::nullopt, navigation_error_to_controller(result.error), false};
  }
  const bool changed = result.changed;
  state = std::move(*result.state);
  return {std::nullopt, SettingsControllerError::none, changed};
}

[[nodiscard]] std::size_t default_selection_for(const Catalog &catalog,
                                                std::string_view node_id) noexcept {
  if (const auto *node = catalog.find_node(node_id))
    return std::min(node->default_selection,
                    node->children.empty() ? std::size_t{} : node->children.size() - 1);
  return 0;
}

} // namespace

SettingsCatalogController::SettingsCatalogController(
    const Catalog &catalog, NavigationState navigation,
    std::size_t previous_selection,
    double focus_transition_started_seconds) noexcept
    : catalog_(&catalog), navigation_(std::move(navigation)),
      previous_selection_(previous_selection),
      focus_transition_started_seconds_(focus_transition_started_seconds) {}

SettingsControllerCreateResult
SettingsCatalogController::create(const Catalog &catalog,
                                  double now_seconds) noexcept {
  const auto *root = catalog.find_node("category.settings");
  if (!root)
    return {std::nullopt, SettingsControllerError::missing_root};
  if (root->children.empty())
    return {std::nullopt, SettingsControllerError::invalid_menu};

  try {
    NavigationState state;
    auto complete = apply_navigation(
        state, {NavigationEventKind::complete_boot, "", 0, true});
    if (!complete)
      return {std::nullopt, complete.error};
    auto select = apply_navigation(
        state, {NavigationEventKind::select_category, "category.settings", 1, true});
    if (!select)
      return {std::nullopt, select.error};
    const auto selection = default_selection_for(catalog, "category.settings");
    auto enter = apply_navigation(
        state,
        {NavigationEventKind::enter_menu, "category.settings", selection, true});
    if (!enter)
      return {std::nullopt, enter.error};
    return {SettingsCatalogController(catalog, std::move(state), selection,
                                      now_seconds),
            SettingsControllerError::none};
  } catch (const std::bad_alloc &) {
    return {std::nullopt, SettingsControllerError::allocation_failure};
  } catch (...) {
    return {std::nullopt, SettingsControllerError::navigation_rejected};
  }
}

const CatalogNode *SettingsCatalogController::active_menu() const noexcept {
  if (!catalog_ || navigation_.stack.empty())
    return nullptr;
  const auto &top = navigation_.top();
  if (top.layer != NavigationLayer::nested_menu ||
      !is_settings_catalog_id(top.id)) {
    return nullptr;
  }
  return catalog_->find_node(top.id);
}

SettingsSceneSample SettingsCatalogController::sample_scene(
    double now_seconds,
    std::span<const SettingsValueOverride> value_overrides) const noexcept {
  if (!catalog_)
    return {.error = SettingsSceneError::menu_not_found};
  const auto input = make_settings_scene_input(
      route(), previous_selection_, focus_transition_started_seconds_);
  return sample_settings_scene(*catalog_, input, now_seconds, value_overrides);
}

SettingsControllerStep
SettingsCatalogController::set_selection(std::size_t selection,
                                         double now_seconds) noexcept {
  const auto *menu = active_menu();
  if (!menu || menu->children.empty())
    return {std::nullopt, SettingsControllerError::invalid_menu, false};
  if (selection >= menu->children.size())
    return {std::nullopt, SettingsControllerError::selection_out_of_range, false};

  const auto old_selection = route().selection;
  auto step = apply_navigation(
      navigation_, {NavigationEventKind::set_selection, "", selection, true});
  if (!step)
    return step;
  if (old_selection != selection) {
    previous_selection_ = old_selection;
    focus_transition_started_seconds_ = now_seconds;
    step.changed = true;
  }
  return step;
}

SettingsControllerStep SettingsCatalogController::move_selection(
    int delta, double now_seconds) noexcept {
  const auto *menu = active_menu();
  if (!menu || menu->children.empty())
    return {std::nullopt, SettingsControllerError::invalid_menu, false};

  const auto current = static_cast<int>(route().selection);
  const auto last = static_cast<int>(menu->children.size() - 1);
  const auto next = std::clamp(current + delta, 0, last);
  return set_selection(static_cast<std::size_t>(next), now_seconds);
}

SettingsControllerStep
SettingsCatalogController::activate(double now_seconds) noexcept {
  const auto *menu = active_menu();
  if (!menu || menu->children.empty())
    return {std::nullopt, SettingsControllerError::invalid_menu, false};
  if (route().selection >= menu->children.size())
    return {std::nullopt, SettingsControllerError::selection_out_of_range, false};

  const auto resolved =
      resolve_settings_action(*catalog_, menu->children[route().selection]);
  if (!resolved || !resolved.plan) {
    return {std::nullopt, SettingsControllerError::action_resolution_failed,
            false};
  }

  SettingsControllerStep step{resolved.plan, SettingsControllerError::none,
                              false};
  if (!resolved.plan->navigation_event) {
    return step;
  }

  const auto selection =
      resolved.plan->kind == SettingsActionKind::navigate_menu
          ? default_selection_for(*catalog_, resolved.plan->target_id)
          : std::size_t{};
  auto nav = apply_navigation(
      navigation_,
      {*resolved.plan->navigation_event, resolved.plan->target_id, selection,
       true});
  if (!nav)
    return {std::nullopt, nav.error, false};

  previous_selection_ = selection;
  focus_transition_started_seconds_ = now_seconds;
  step.changed = nav.changed;
  return step;
}

SettingsControllerStep
SettingsCatalogController::back(double now_seconds) noexcept {
  auto step =
      apply_navigation(navigation_, {NavigationEventKind::back, "", 0, true});
  if (!step)
    return step;
  previous_selection_ = route().selection;
  focus_transition_started_seconds_ = now_seconds;
  return step;
}

} // namespace openxmb::xmb
