#pragma once

#include "openxmb/dialogs/model.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openxmb::dialogs {

enum class FocusKind { none, choice, button, field };

struct FocusTarget {
  FocusKind kind{FocusKind::none};
  std::string id;

  friend bool operator==(const FocusTarget &, const FocusTarget &) = default;
};

struct FrameState {
  std::string definition_id;
  FocusTarget focus;
  std::size_t focus_position{};
  std::size_t selected_choice{};
  std::map<std::string, std::string, std::less<>> fields;
  double progress{};
  std::uint64_t entered_at_ms{};

  friend bool operator==(const FrameState &, const FrameState &) = default;
};

enum class OutcomeKind { none, completed, cancelled, failed };

struct Outcome {
  OutcomeKind kind{OutcomeKind::none};
  std::string definition_id;
  std::string action_id;
  std::string result_code;
  std::string message_key;

  friend bool operator==(const Outcome &, const Outcome &) = default;
};

struct SessionState {
  std::vector<FrameState> stack;
  std::map<std::string, std::string, std::less<>> variables;
  Outcome outcome;
  std::uint64_t revision{};

  [[nodiscard]] bool active() const noexcept {
    return outcome.kind == OutcomeKind::none && !stack.empty();
  }
  [[nodiscard]] const FrameState *top() const noexcept {
    return stack.empty() ? nullptr : &stack.back();
  }
  [[nodiscard]] std::string_view focus_owner() const noexcept {
    return stack.empty() ? std::string_view{} : stack.back().definition_id;
  }

  friend bool operator==(const SessionState &, const SessionState &) = default;
};

enum class EventKind {
  navigate,
  set_focus,
  set_field,
  set_progress,
  confirm,
  cancel,
  complete_progress,
  set_variable,
  fail,
};

struct Event {
  EventKind kind{EventKind::confirm};
  int delta{};
  std::string id;
  std::string value;
  double progress{};
  std::string result_code;
  std::string message_key;

  Event() = default;
  Event(EventKind event_kind, int event_delta = 0, std::string event_id = {},
        std::string event_value = {}, double event_progress = 0.0,
        std::string event_result_code = {}, std::string event_message_key = {})
      : kind(event_kind), delta(event_delta), id(std::move(event_id)),
        value(std::move(event_value)), progress(event_progress),
        result_code(std::move(event_result_code)),
        message_key(std::move(event_message_key)) {}
};

enum class MachineError {
  none,
  invalid_catalog,
  invalid_state,
  missing_definition,
  invalid_event,
  invalid_focus,
  validation_failed,
  guard_rejected,
  progress_incomplete,
  allocation_failure,
};

enum class ValidationErrorCode {
  required,
  too_short,
  too_long,
  invalid_characters,
  invalid_ipv4,
};

struct ValidationIssue {
  std::string field_id;
  ValidationErrorCode code{ValidationErrorCode::required};
  std::string message_key;

  friend bool operator==(const ValidationIssue &,
                         const ValidationIssue &) = default;
};

struct TransitionResult {
  std::optional<SessionState> state;
  MachineError error{MachineError::none};
  bool changed{};
  std::vector<ValidationIssue> validation;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == MachineError::none;
  }
};

struct AnimationSample {
  double reveal_linear{};
  double reveal_eased{};
  double opacity{};
  double scale{1.0};
  double backdrop_opacity{};
  double spinner_phase{};
  double progress{};

  friend bool operator==(const AnimationSample &,
                         const AnimationSample &) = default;
};

[[nodiscard]] TransitionResult start(const DefinitionCatalog &catalog,
                                     std::string_view definition_id,
                                     std::uint64_t now_ms) noexcept;

// Pure transition. Failures never mutate or return a replacement state.
[[nodiscard]] TransitionResult transition(const DefinitionCatalog &catalog,
                                          const SessionState &state,
                                          const Event &event,
                                          std::uint64_t now_ms) noexcept;

[[nodiscard]] std::vector<ValidationIssue>
validate_fields(const ScreenDefinition &definition, const FrameState &frame);

[[nodiscard]] AnimationSample
sample_animation(const ScreenDefinition &definition, const FrameState &frame,
                 std::uint64_t now_ms) noexcept;

} // namespace openxmb::dialogs
