#include "openxmb/dialogs/model.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_set>

namespace openxmb::dialogs {
namespace {

void append_error(std::vector<DefinitionError> &errors,
                  DefinitionErrorCode code, const ScreenDefinition &definition,
                  std::string member, std::string message) {
  errors.push_back(
      {code, definition.id, std::move(member), std::move(message)});
}

template <class Entry>
bool has_duplicate_or_empty_id(const std::vector<Entry> &entries,
                               const ScreenDefinition &definition,
                               std::string_view collection,
                               std::vector<DefinitionError> &errors) {
  std::unordered_set<std::string> ids;
  bool invalid = false;
  for (const auto &entry : entries) {
    if (entry.id.empty()) {
      append_error(errors, DefinitionErrorCode::empty_identifier, definition,
                   std::string(collection), "entry id must not be empty");
      invalid = true;
    } else if (!ids.insert(entry.id).second) {
      append_error(errors, DefinitionErrorCode::duplicate_identifier,
                   definition, entry.id,
                   "entry id must be unique within its collection");
      invalid = true;
    }
  }
  return invalid;
}

bool contains_action(const ScreenDefinition &definition,
                     std::string_view id) noexcept {
  return std::ranges::any_of(
             definition.choices,
             [&](const auto &entry) { return entry.id == id; }) ||
         std::ranges::any_of(definition.buttons,
                             [&](const auto &entry) { return entry.id == id; });
}

} // namespace

bool DefinitionCatalog::add(ScreenDefinition definition) {
  if (definition.id.empty())
    return false;
  return definitions_.emplace(definition.id, std::move(definition)).second;
}

const ScreenDefinition *
DefinitionCatalog::find(const std::string_view id) const noexcept {
  const auto found = std::ranges::find_if(
      definitions_, [&](const auto &entry) { return entry.first == id; });
  return found == definitions_.end() ? nullptr : &found->second;
}

std::size_t DefinitionCatalog::dialog_count() const noexcept {
  return static_cast<std::size_t>(
      std::ranges::count_if(definitions_, [](const auto &entry) {
        return entry.second.surface == SurfaceKind::dialog;
      }));
}

std::size_t DefinitionCatalog::wizard_screen_count() const noexcept {
  return static_cast<std::size_t>(
      std::ranges::count_if(definitions_, [](const auto &entry) {
        return entry.second.surface == SurfaceKind::wizard;
      }));
}

std::vector<DefinitionError> DefinitionCatalog::validate() const {
  std::vector<DefinitionError> errors;
  for (const auto &[map_id, definition] : definitions_) {
    if (definition.id.empty() || definition.id != map_id) {
      append_error(errors, DefinitionErrorCode::empty_identifier, definition,
                   {}, "definition id is empty or inconsistent");
      continue;
    }

    has_duplicate_or_empty_id(definition.choices, definition, "choices",
                              errors);
    has_duplicate_or_empty_id(definition.buttons, definition, "buttons",
                              errors);
    has_duplicate_or_empty_id(definition.fields, definition, "fields", errors);

    std::unordered_set<std::string> all_focus_ids;
    for (const auto &choice : definition.choices)
      all_focus_ids.insert(choice.id);
    for (const auto &button : definition.buttons) {
      if (!all_focus_ids.insert(button.id).second) {
        append_error(errors, DefinitionErrorCode::duplicate_identifier,
                     definition, button.id,
                     "choice and button ids share one focus namespace");
      }
    }
    for (const auto &field : definition.fields) {
      if (!all_focus_ids.insert(field.id).second) {
        append_error(errors, DefinitionErrorCode::duplicate_identifier,
                     definition, field.id,
                     "field id duplicates another focus target");
      }
      if (field.minimum_length > field.maximum_length ||
          field.maximum_length > 65536) {
        append_error(errors, DefinitionErrorCode::invalid_shape, definition,
                     field.id, "field length bounds are invalid");
      }
    }

    const auto default_buttons = static_cast<std::size_t>(
        std::ranges::count_if(definition.buttons, [](const auto &button) {
          return button.is_default && button.enabled;
        }));
    const auto disabled_defaults =
        std::ranges::any_of(definition.buttons, [](const auto &button) {
          return button.is_default && !button.enabled;
        });
    const auto enabled_buttons = static_cast<std::size_t>(std::ranges::count_if(
        definition.buttons, [](const auto &button) { return button.enabled; }));
    if ((enabled_buttons != 0 && default_buttons != 1) || disabled_defaults) {
      append_error(errors, DefinitionErrorCode::invalid_default, definition, {},
                   "a button set requires exactly one enabled default");
    }
    if (!definition.choices.empty() &&
        definition.default_selection >= definition.choices.size()) {
      append_error(errors, DefinitionErrorCode::invalid_default, definition, {},
                   "default choice index is out of range");
    } else if (definition.choices.empty() &&
               definition.default_selection != 0) {
      append_error(errors, DefinitionErrorCode::invalid_default, definition, {},
                   "screen without choices must use default index zero");
    }

    if (definition.kind == ScreenKind::choice && definition.choices.empty()) {
      append_error(errors, DefinitionErrorCode::invalid_shape, definition, {},
                   "choice screen requires at least one choice");
    }
    if (definition.kind == ScreenKind::text_entry &&
        definition.fields.empty()) {
      append_error(errors, DefinitionErrorCode::invalid_shape, definition, {},
                   "text-entry screen requires at least one field");
    }
    if ((definition.kind == ScreenKind::progress ||
         definition.kind == ScreenKind::busy) &&
        (!definition.fields.empty() || !definition.choices.empty())) {
      append_error(errors, DefinitionErrorCode::invalid_shape, definition, {},
                   "progress and busy screens cannot own fields or choices");
    }
    if (definition.timeline.reveal_duration_ms == 0 ||
        definition.timeline.spinner_period_ms == 0 ||
        !std::isfinite(definition.timeline.initial_scale) ||
        definition.timeline.initial_scale <= 0.0 ||
        !std::isfinite(definition.timeline.backdrop_opacity) ||
        definition.timeline.backdrop_opacity < 0.0 ||
        definition.timeline.backdrop_opacity > 1.0) {
      append_error(errors, DefinitionErrorCode::invalid_timeline, definition,
                   {}, "timeline values must be finite and positive");
    }

    std::set<std::pair<RouteTrigger, std::string>> route_keys;
    for (const auto &route : definition.routes) {
      if (!route_keys.emplace(route.trigger, route.action_id).second) {
        append_error(errors, DefinitionErrorCode::duplicate_identifier,
                     definition, route.action_id,
                     "route trigger and action pair must be unique");
      }
      if (!route.action_id.empty() && route.trigger != RouteTrigger::confirm) {
        append_error(errors, DefinitionErrorCode::invalid_route, definition,
                     route.action_id,
                     "only confirm routes can select an action id");
      } else if (!route.action_id.empty() &&
                 !contains_action(definition, route.action_id)) {
        append_error(errors, DefinitionErrorCode::invalid_route, definition,
                     route.action_id,
                     "route action does not name a choice or button");
      }
      if (!std::isfinite(route.minimum_progress) ||
          route.minimum_progress < 0.0 || route.minimum_progress > 1.0) {
        append_error(errors, DefinitionErrorCode::invalid_route, definition,
                     route.action_id,
                     "route progress threshold must be within zero and one");
      }
      const bool needs_target = route.operation == RouteOperation::push ||
                                route.operation == RouteOperation::replace;
      if (needs_target) {
        if (route.target_id.empty()) {
          append_error(errors, DefinitionErrorCode::invalid_route, definition,
                       route.action_id, "route target must not be empty");
        } else if (!find(route.target_id)) {
          append_error(errors, DefinitionErrorCode::unresolved_target,
                       definition, route.target_id,
                       "route target does not exist in the catalog");
        }
      } else if (!route.target_id.empty()) {
        append_error(errors, DefinitionErrorCode::invalid_route, definition,
                     route.target_id,
                     "only push and replace routes may name a target");
      }
      if (route.operation == RouteOperation::fail &&
          route.result_code.empty()) {
        append_error(errors, DefinitionErrorCode::invalid_route, definition,
                     route.action_id,
                     "failure route requires a stable result code");
      }
      for (const auto &guard : route.guards) {
        if (guard.variable.empty()) {
          append_error(errors, DefinitionErrorCode::invalid_route, definition,
                       route.action_id,
                       "route guard variable must not be empty");
        }
      }
    }

    const auto has_wildcard_confirm =
        std::ranges::any_of(definition.routes, [](const auto &route) {
          return route.trigger == RouteTrigger::confirm &&
                 route.action_id.empty();
        });
    const auto has_confirm_route = [&](const std::string_view action_id) {
      return has_wildcard_confirm ||
             std::ranges::any_of(definition.routes, [&](const auto &route) {
               return route.trigger == RouteTrigger::confirm &&
                      route.action_id == action_id;
             });
    };
    for (const auto &choice : definition.choices) {
      if (!has_confirm_route(choice.id)) {
        append_error(errors, DefinitionErrorCode::invalid_route, definition,
                     choice.id, "focusable choice has no confirm route");
      }
    }
    for (const auto &button : definition.buttons) {
      if (button.enabled && !has_confirm_route(button.id)) {
        append_error(errors, DefinitionErrorCode::invalid_route, definition,
                     button.id, "enabled button has no confirm route");
      }
    }
  }
  return errors;
}

} // namespace openxmb::dialogs
