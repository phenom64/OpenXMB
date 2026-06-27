#include "openxmb/dialogs/state_machine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <new>

namespace openxmb::dialogs {
namespace {

std::vector<FocusTarget> focus_targets(const ScreenDefinition &definition) {
  std::vector<FocusTarget> targets;
  targets.reserve(definition.fields.size() + definition.choices.size() +
                  definition.buttons.size());

  if (definition.kind == ScreenKind::text_entry) {
    for (const auto &field : definition.fields)
      targets.push_back({FocusKind::field, field.id});
  }
  for (const auto &choice : definition.choices)
    targets.push_back({FocusKind::choice, choice.id});
  for (const auto &button : definition.buttons) {
    if (button.enabled)
      targets.push_back({FocusKind::button, button.id});
  }
  return targets;
}

std::optional<std::size_t>
default_focus_position(const ScreenDefinition &definition,
                       const std::vector<FocusTarget> &targets) {
  if (targets.empty())
    return std::nullopt;
  if (definition.kind == ScreenKind::text_entry) {
    const auto found = std::ranges::find_if(targets, [](const auto &target) {
      return target.kind == FocusKind::field;
    });
    if (found != targets.end())
      return static_cast<std::size_t>(found - targets.begin());
  }
  if (!definition.choices.empty()) {
    const auto &id = definition.choices[definition.default_selection].id;
    const auto found = std::ranges::find_if(
        targets, [&](const auto &target) { return target.id == id; });
    if (found != targets.end())
      return static_cast<std::size_t>(found - targets.begin());
  }
  const auto default_button =
      std::ranges::find_if(definition.buttons, [](const auto &button) {
        return button.enabled && button.is_default;
      });
  if (default_button != definition.buttons.end()) {
    const auto found = std::ranges::find_if(targets, [&](const auto &target) {
      return target.id == default_button->id;
    });
    if (found != targets.end())
      return static_cast<std::size_t>(found - targets.begin());
  }
  return 0;
}

FrameState make_frame(const ScreenDefinition &definition,
                      const std::uint64_t now_ms) {
  FrameState frame;
  frame.definition_id = definition.id;
  frame.selected_choice = definition.default_selection;
  frame.entered_at_ms = now_ms;
  for (const auto &field : definition.fields)
    frame.fields.emplace(field.id, field.initial_value);
  const auto targets = focus_targets(definition);
  if (const auto position = default_focus_position(definition, targets)) {
    frame.focus_position = *position;
    frame.focus = targets[*position];
  }
  return frame;
}

bool is_valid_ipv4(std::string_view value) noexcept {
  if (value.empty())
    return false;
  std::size_t part_count = 0;
  std::size_t begin = 0;
  while (begin <= value.size()) {
    const auto end = value.find('.', begin);
    const auto stop = end == std::string_view::npos ? value.size() : end;
    const auto part = value.substr(begin, stop - begin);
    if (part.empty() || part.size() > 3)
      return false;
    unsigned number = 0;
    for (const char raw_character : part) {
      const auto character = static_cast<unsigned char>(raw_character);
      if (!std::isdigit(character))
        return false;
      number = number * 10U + static_cast<unsigned>(character - '0');
    }
    if (number > 255U)
      return false;
    ++part_count;
    if (end == std::string_view::npos)
      break;
    begin = end + 1;
  }
  return part_count == 4;
}

bool matches_policy(const std::string_view value,
                    const CharacterPolicy policy) noexcept {
  switch (policy) {
  case CharacterPolicy::any:
    return true;
  case CharacterPolicy::printable_ascii:
    return std::ranges::all_of(value, [](const unsigned char character) {
      return character >= 0x20U && character <= 0x7eU;
    });
  case CharacterPolicy::numeric:
    return std::ranges::all_of(value, [](const unsigned char character) {
      return std::isdigit(character) != 0;
    });
  case CharacterPolicy::hexadecimal:
    return std::ranges::all_of(value, [](const unsigned char character) {
      return std::isxdigit(character) != 0;
    });
  case CharacterPolicy::ipv4:
    return is_valid_ipv4(value);
  }
  return false;
}

bool valid_state(const DefinitionCatalog &catalog, const SessionState &state) {
  if (state.outcome.kind != OutcomeKind::none)
    return state.stack.empty();
  if (state.stack.empty())
    return false;
  for (const auto &frame : state.stack) {
    const auto *definition = catalog.find(frame.definition_id);
    if (!definition || !std::isfinite(frame.progress) || frame.progress < 0.0 ||
        frame.progress > 1.0)
      return false;
    if (!definition->choices.empty() &&
        frame.selected_choice >= definition->choices.size())
      return false;
    if (definition->choices.empty() && frame.selected_choice != 0)
      return false;
    if (frame.fields.size() != definition->fields.size())
      return false;
    for (const auto &field : definition->fields) {
      if (!frame.fields.contains(field.id))
        return false;
    }
    const auto targets = focus_targets(*definition);
    if (targets.empty()) {
      if (frame.focus.kind != FocusKind::none || !frame.focus.id.empty() ||
          frame.focus_position != 0)
        return false;
    } else if (frame.focus_position >= targets.size() ||
               frame.focus != targets[frame.focus_position]) {
      return false;
    }
    if (frame.focus.kind == FocusKind::choice) {
      const auto choice =
          std::ranges::find_if(definition->choices, [&](const auto &entry) {
            return entry.id == frame.focus.id;
          });
      if (choice == definition->choices.end() ||
          frame.selected_choice !=
              static_cast<std::size_t>(choice - definition->choices.begin()))
        return false;
    }
  }
  return true;
}

const ButtonDefinition *
default_button(const ScreenDefinition &definition) noexcept {
  const auto button =
      std::ranges::find_if(definition.buttons, [](const auto &entry) {
        return entry.enabled && entry.is_default;
      });
  if (button != definition.buttons.end())
    return &*button;
  const auto first = std::ranges::find_if(
      definition.buttons, [](const auto &entry) { return entry.enabled; });
  return first == definition.buttons.end() ? nullptr : &*first;
}

const RouteDefinition *find_route(const ScreenDefinition &definition,
                                  const RouteTrigger trigger,
                                  const std::string_view action_id) noexcept {
  const auto exact =
      std::ranges::find_if(definition.routes, [&](const auto &route) {
        return route.trigger == trigger && !route.action_id.empty() &&
               route.action_id == action_id;
      });
  if (exact != definition.routes.end())
    return &*exact;
  const auto wildcard =
      std::ranges::find_if(definition.routes, [&](const auto &route) {
        return route.trigger == trigger && route.action_id.empty();
      });
  return wildcard == definition.routes.end() ? nullptr : &*wildcard;
}

bool guards_pass(const RouteDefinition &route,
                 const SessionState &state) noexcept {
  for (const auto &guard : route.guards) {
    const auto found = state.variables.find(guard.variable);
    const bool equal =
        found != state.variables.end() && found->second == guard.expected_value;
    if (equal == guard.negate)
      return false;
  }
  return true;
}

void skip_non_resumable(const DefinitionCatalog &catalog, SessionState &state) {
  while (state.stack.size() > 1) {
    const auto *definition = catalog.find(state.stack.back().definition_id);
    if (!definition || definition->resumable)
      break;
    state.stack.pop_back();
  }
}

TransitionResult apply_route(const DefinitionCatalog &catalog,
                             const SessionState &original, SessionState next,
                             const ScreenDefinition &definition,
                             const RouteDefinition &route,
                             const std::string_view action_id,
                             const std::uint64_t now_ms) {
  if (!guards_pass(route, next))
    return {std::nullopt, MachineError::guard_rejected, false, {}};
  if (route.require_valid_fields) {
    auto issues = validate_fields(definition, next.stack.back());
    if (!issues.empty()) {
      return {std::nullopt, MachineError::validation_failed, false,
              std::move(issues)};
    }
  }
  if (next.stack.back().progress + std::numeric_limits<double>::epsilon() <
      route.minimum_progress) {
    return {std::nullopt, MachineError::progress_incomplete, false, {}};
  }

  switch (route.operation) {
  case RouteOperation::stay:
    break;
  case RouteOperation::push: {
    const auto *target = catalog.find(route.target_id);
    if (!target)
      return {std::nullopt, MachineError::missing_definition, false, {}};
    next.stack.push_back(make_frame(*target, now_ms));
    break;
  }
  case RouteOperation::replace: {
    const auto *target = catalog.find(route.target_id);
    if (!target)
      return {std::nullopt, MachineError::missing_definition, false, {}};
    next.stack.back() = make_frame(*target, now_ms);
    break;
  }
  case RouteOperation::pop:
    if (next.stack.size() <= 1)
      return {std::nullopt, MachineError::invalid_event, false, {}};
    next.stack.pop_back();
    skip_non_resumable(catalog, next);
    break;
  case RouteOperation::complete:
  case RouteOperation::cancel:
  case RouteOperation::fail:
    next.outcome.kind =
        route.operation == RouteOperation::complete ? OutcomeKind::completed
        : route.operation == RouteOperation::cancel ? OutcomeKind::cancelled
                                                    : OutcomeKind::failed;
    next.outcome.definition_id = definition.id;
    next.outcome.action_id = std::string(action_id);
    next.outcome.result_code = route.result_code;
    next.outcome.message_key = route.message_key;
    next.stack.clear();
    break;
  }
  const bool changed = next != original;
  if (changed)
    ++next.revision;
  if (!valid_state(catalog, next))
    return {std::nullopt, MachineError::invalid_state, false, {}};
  return {std::move(next), MachineError::none, changed, {}};
}

} // namespace

std::vector<ValidationIssue> validate_fields(const ScreenDefinition &definition,
                                             const FrameState &frame) {
  std::vector<ValidationIssue> issues;
  for (const auto &field : definition.fields) {
    const auto found = frame.fields.find(field.id);
    const auto value = found == frame.fields.end()
                           ? std::string_view{}
                           : std::string_view(found->second);
    if (field.required && value.empty()) {
      issues.push_back({field.id, ValidationErrorCode::required,
                        "dialog.validation.required"});
      continue;
    }
    if (value.empty())
      continue;
    if (value.size() < field.minimum_length) {
      issues.push_back({field.id, ValidationErrorCode::too_short,
                        "dialog.validation.too-short"});
    } else if (value.size() > field.maximum_length) {
      issues.push_back({field.id, ValidationErrorCode::too_long,
                        "dialog.validation.too-long"});
    } else if (!value.empty() &&
               !matches_policy(value, field.character_policy)) {
      const auto code = field.character_policy == CharacterPolicy::ipv4
                            ? ValidationErrorCode::invalid_ipv4
                            : ValidationErrorCode::invalid_characters;
      issues.push_back({field.id, code,
                        code == ValidationErrorCode::invalid_ipv4
                            ? "dialog.validation.invalid-ipv4"
                            : "dialog.validation.invalid-characters"});
    }
  }
  return issues;
}

TransitionResult start(const DefinitionCatalog &catalog,
                       const std::string_view definition_id,
                       const std::uint64_t now_ms) noexcept {
  try {
    if (!catalog.validate().empty())
      return {std::nullopt, MachineError::invalid_catalog, false, {}};
    const auto *definition = catalog.find(definition_id);
    if (!definition)
      return {std::nullopt, MachineError::missing_definition, false, {}};
    SessionState state;
    state.stack.push_back(make_frame(*definition, now_ms));
    if (!valid_state(catalog, state))
      return {std::nullopt, MachineError::invalid_state, false, {}};
    return {std::move(state), MachineError::none, true, {}};
  } catch (const std::bad_alloc &) {
    return {std::nullopt, MachineError::allocation_failure, false, {}};
  } catch (...) {
    return {std::nullopt, MachineError::invalid_catalog, false, {}};
  }
}

TransitionResult transition(const DefinitionCatalog &catalog,
                            const SessionState &state, const Event &event,
                            const std::uint64_t now_ms) noexcept {
  try {
    if (!valid_state(catalog, state))
      return {std::nullopt, MachineError::invalid_state, false, {}};
    SessionState next = state;
    auto &frame = next.stack.back();
    const auto *definition = catalog.find(frame.definition_id);
    if (!definition)
      return {std::nullopt, MachineError::missing_definition, false, {}};

    switch (event.kind) {
    case EventKind::navigate: {
      if (event.delta == 0)
        return {std::move(next), MachineError::none, false, {}};
      const auto targets = focus_targets(*definition);
      if (targets.empty())
        return {std::nullopt, MachineError::invalid_focus, false, {}};
      const auto count = static_cast<long long>(targets.size());
      auto position = static_cast<long long>(frame.focus_position);
      position = (position + static_cast<long long>(event.delta)) % count;
      if (position < 0)
        position += count;
      frame.focus_position = static_cast<std::size_t>(position);
      frame.focus = targets[frame.focus_position];
      if (frame.focus.kind == FocusKind::choice) {
        const auto choice =
            std::ranges::find_if(definition->choices, [&](const auto &entry) {
              return entry.id == frame.focus.id;
            });
        frame.selected_choice =
            static_cast<std::size_t>(choice - definition->choices.begin());
      }
      break;
    }
    case EventKind::set_focus: {
      const auto targets = focus_targets(*definition);
      const auto found = std::ranges::find_if(
          targets, [&](const auto &target) { return target.id == event.id; });
      if (found == targets.end())
        return {std::nullopt, MachineError::invalid_focus, false, {}};
      frame.focus_position = static_cast<std::size_t>(found - targets.begin());
      frame.focus = *found;
      if (frame.focus.kind == FocusKind::choice) {
        const auto choice =
            std::ranges::find_if(definition->choices, [&](const auto &entry) {
              return entry.id == frame.focus.id;
            });
        frame.selected_choice =
            static_cast<std::size_t>(choice - definition->choices.begin());
      }
      break;
    }
    case EventKind::set_field: {
      const auto field =
          std::ranges::find_if(definition->fields, [&](const auto &entry) {
            return entry.id == event.id;
          });
      if (field == definition->fields.end() || event.value.size() > 65536)
        return {std::nullopt, MachineError::invalid_event, false, {}};
      frame.fields[event.id] = event.value;
      break;
    }
    case EventKind::set_progress:
      if ((definition->kind != ScreenKind::progress &&
           definition->kind != ScreenKind::busy) ||
          !std::isfinite(event.progress) || event.progress < 0.0 ||
          event.progress > 1.0) {
        return {std::nullopt, MachineError::invalid_event, false, {}};
      }
      frame.progress = event.progress;
      break;
    case EventKind::set_variable:
      if (event.id.empty() || event.id.size() > 1024 ||
          event.value.size() > 65536) {
        return {std::nullopt, MachineError::invalid_event, false, {}};
      }
      next.variables[event.id] = event.value;
      break;
    case EventKind::fail:
      if (event.result_code.empty())
        return {std::nullopt, MachineError::invalid_event, false, {}};
      next.outcome = {OutcomeKind::failed,
                      definition->id,
                      {},
                      event.result_code,
                      event.message_key};
      next.stack.clear();
      break;
    case EventKind::confirm: {
      std::string_view action_id;
      if (frame.focus.kind == FocusKind::choice ||
          frame.focus.kind == FocusKind::button) {
        action_id = frame.focus.id;
      } else if (const auto *button = default_button(*definition)) {
        action_id = button->id;
      }
      const auto *route =
          find_route(*definition, RouteTrigger::confirm, action_id);
      if (!route)
        return {std::nullopt, MachineError::invalid_event, false, {}};
      return apply_route(catalog, state, std::move(next), *definition, *route,
                         action_id, now_ms);
    }
    case EventKind::cancel: {
      if (!definition->cancel_allowed)
        return {std::nullopt, MachineError::invalid_event, false, {}};
      if (const auto *route =
              find_route(*definition, RouteTrigger::cancel, {})) {
        return apply_route(catalog, state, std::move(next), *definition, *route,
                           {}, now_ms);
      }
      if (next.stack.size() > 1) {
        next.stack.pop_back();
        skip_non_resumable(catalog, next);
      } else {
        next.outcome = {
            OutcomeKind::cancelled, definition->id, {}, "cancelled", {}};
        next.stack.clear();
      }
      break;
    }
    case EventKind::complete_progress: {
      if (definition->kind != ScreenKind::progress &&
          definition->kind != ScreenKind::busy) {
        return {std::nullopt, MachineError::invalid_event, false, {}};
      }
      const auto *route =
          find_route(*definition, RouteTrigger::progress_complete, {});
      if (!route)
        return {std::nullopt, MachineError::invalid_event, false, {}};
      return apply_route(catalog, state, std::move(next), *definition, *route,
                         {}, now_ms);
    }
    }

    const bool changed = next != state;
    if (changed)
      ++next.revision;
    if (!valid_state(catalog, next))
      return {std::nullopt, MachineError::invalid_state, false, {}};
    return {std::move(next), MachineError::none, changed, {}};
  } catch (const std::bad_alloc &) {
    return {std::nullopt, MachineError::allocation_failure, false, {}};
  } catch (...) {
    return {std::nullopt, MachineError::invalid_event, false, {}};
  }
}

AnimationSample sample_animation(const ScreenDefinition &definition,
                                 const FrameState &frame,
                                 const std::uint64_t now_ms) noexcept {
  const auto elapsed = now_ms > frame.entered_at_ms
                           ? now_ms - frame.entered_at_ms
                           : std::uint64_t{};
  double linear = 0.0;
  if (elapsed > definition.timeline.reveal_delay_ms) {
    const auto active = elapsed - definition.timeline.reveal_delay_ms;
    const auto duration =
        std::max<std::uint64_t>(definition.timeline.reveal_duration_ms, 1);
    linear = std::clamp(
        static_cast<double>(active) / static_cast<double>(duration), 0.0, 1.0);
  }
  const auto inverse = 1.0 - linear;
  const auto eased = 1.0 - inverse * inverse * inverse;
  const auto spinner_period =
      std::max<std::uint64_t>(definition.timeline.spinner_period_ms, 1);
  const auto spinner_elapsed = elapsed % spinner_period;
  const auto spinner_phase = static_cast<double>(spinner_elapsed) /
                             static_cast<double>(spinner_period);
  return {.reveal_linear = linear,
          .reveal_eased = eased,
          .opacity = eased,
          .scale = definition.timeline.initial_scale +
                   (1.0 - definition.timeline.initial_scale) * eased,
          .backdrop_opacity = definition.timeline.backdrop_opacity * eased,
          .spinner_phase = spinner_phase,
          .progress = frame.progress};
}

} // namespace openxmb::dialogs
