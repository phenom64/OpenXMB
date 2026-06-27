#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openxmb::dialogs {

enum class SurfaceKind { dialog, wizard };

enum class ScreenKind {
  message,
  choice,
  progress,
  busy,
  error,
  text_entry,
  information,
};

enum class ButtonRole { accept, cancel, destructive, auxiliary };

enum class CharacterPolicy { any, printable_ascii, numeric, hexadecimal, ipv4 };

enum class RouteTrigger { confirm, cancel, progress_complete };

enum class RouteOperation {
  stay,
  push,
  replace,
  pop,
  complete,
  cancel,
  fail,
};

struct ChoiceDefinition {
  std::string id;
  std::string label_key;
  bool destructive{};

  friend bool operator==(const ChoiceDefinition &,
                         const ChoiceDefinition &) = default;
};

struct ButtonDefinition {
  std::string id;
  std::string label_key;
  ButtonRole role{ButtonRole::accept};
  bool is_default{};
  bool enabled{true};

  friend bool operator==(const ButtonDefinition &,
                         const ButtonDefinition &) = default;
};

struct FieldDefinition {
  std::string id;
  std::string label_key;
  std::string initial_value;
  bool required{};
  std::size_t minimum_length{};
  std::size_t maximum_length{4096};
  CharacterPolicy character_policy{CharacterPolicy::any};

  friend bool operator==(const FieldDefinition &,
                         const FieldDefinition &) = default;
};

struct RouteGuard {
  std::string variable;
  std::string expected_value;
  bool negate{};

  friend bool operator==(const RouteGuard &, const RouteGuard &) = default;
};

struct RouteDefinition {
  RouteTrigger trigger{RouteTrigger::confirm};
  // Empty action_id is a wildcard. Otherwise it matches the focused choice or
  // button id. Exact matches always win over a wildcard route.
  std::string action_id;
  RouteOperation operation{RouteOperation::stay};
  std::string target_id;
  std::string result_code;
  std::string message_key;
  std::vector<RouteGuard> guards;
  bool require_valid_fields{true};
  double minimum_progress{};

  friend bool operator==(const RouteDefinition &,
                         const RouteDefinition &) = default;
};

struct TimelineDefinition {
  std::uint64_t reveal_delay_ms{};
  std::uint64_t reveal_duration_ms{250};
  std::uint64_t spinner_period_ms{1000};
  double initial_scale{0.985};
  double backdrop_opacity{0.58};

  friend bool operator==(const TimelineDefinition &,
                         const TimelineDefinition &) = default;
};

struct ScreenDefinition {
  std::string id;
  SurfaceKind surface{SurfaceKind::dialog};
  ScreenKind kind{ScreenKind::message};
  std::string title_key;
  std::string body_key;
  std::string illustration_ref;
  std::vector<ChoiceDefinition> choices;
  std::vector<ButtonDefinition> buttons;
  std::vector<FieldDefinition> fields;
  std::vector<RouteDefinition> routes;
  std::size_t default_selection{};
  bool resumable{true};
  bool cancel_allowed{true};
  TimelineDefinition timeline;

  friend bool operator==(const ScreenDefinition &,
                         const ScreenDefinition &) = default;
};

enum class DefinitionErrorCode {
  none,
  empty_identifier,
  duplicate_identifier,
  invalid_shape,
  invalid_default,
  invalid_route,
  unresolved_target,
  invalid_timeline,
};

struct DefinitionError {
  DefinitionErrorCode code{DefinitionErrorCode::none};
  std::string definition_id;
  std::string member_id;
  std::string message;
};

class DefinitionCatalog {
public:
  [[nodiscard]] bool add(ScreenDefinition definition);
  [[nodiscard]] const ScreenDefinition *
  find(std::string_view id) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept {
    return definitions_.size();
  }
  [[nodiscard]] std::size_t dialog_count() const noexcept;
  [[nodiscard]] std::size_t wizard_screen_count() const noexcept;
  [[nodiscard]] std::vector<DefinitionError> validate() const;

private:
  std::unordered_map<std::string, ScreenDefinition> definitions_;
};

} // namespace openxmb::dialogs
