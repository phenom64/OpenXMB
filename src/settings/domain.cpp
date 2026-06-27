#include "openxmb/settings/domain.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_set>

namespace openxmb::settings {
namespace {

[[nodiscard]] SettingsError
error(SettingsErrorCode code, std::string_view subject, std::string detail) {
  return {code, std::string(subject), std::move(detail)};
}

[[nodiscard]] bool is_semantic_id(std::string_view value) noexcept {
  if (value.empty())
    return false;
  bool previous_separator = true;
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    const bool alphanumeric = (character >= static_cast<unsigned char>('a') &&
                               character <= static_cast<unsigned char>('z')) ||
                              (character >= static_cast<unsigned char>('0') &&
                               character <= static_cast<unsigned char>('9'));
    const bool separator = character == static_cast<unsigned char>('.') ||
                           character == static_cast<unsigned char>('-') ||
                           character == static_cast<unsigned char>('_');
    if ((!alphanumeric && !separator) || (separator && previous_separator))
      return false;
    previous_separator = separator;
  }
  return !previous_separator;
}

[[nodiscard]] std::optional<std::size_t>
utf8_code_points(std::string_view text) noexcept {
  std::size_t count = 0;
  for (std::size_t index = 0; index < text.size();) {
    const auto first = static_cast<unsigned char>(text[index]);
    std::size_t length = 0;
    std::uint32_t code_point = 0;
    if (first <= 0x7fU) {
      length = 1;
      code_point = first;
    } else if (first >= 0xc2U && first <= 0xdfU) {
      length = 2;
      code_point = first & 0x1fU;
    } else if (first >= 0xe0U && first <= 0xefU) {
      length = 3;
      code_point = first & 0x0fU;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      length = 4;
      code_point = first & 0x07U;
    } else {
      return std::nullopt;
    }
    if (index + length > text.size())
      return std::nullopt;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto continuation =
          static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xc0U) != 0x80U)
        return std::nullopt;
      code_point = (code_point << 6U) | (continuation & 0x3fU);
    }
    if ((length == 3 && code_point < 0x800U) ||
        (length == 4 && code_point < 0x10000U) ||
        (code_point >= 0xd800U && code_point <= 0xdfffU) ||
        code_point > 0x10ffffU) {
      return std::nullopt;
    }
    index += length;
    ++count;
  }
  return count;
}

[[nodiscard]] bool integer_aligned(std::int64_t value,
                                   const IntegerConstraint &constraint) {
  const auto offset = static_cast<std::uint64_t>(value) -
                      static_cast<std::uint64_t>(constraint.minimum);
  return offset % static_cast<std::uint64_t>(constraint.step) == 0;
}

[[nodiscard]] bool floating_aligned(double value,
                                    const FloatConstraint &constraint) {
  const double quotient = (value - constraint.minimum) / constraint.step;
  const double nearest = std::round(quotient);
  const double tolerance = 1.0e-9 * std::max(1.0, std::abs(quotient));
  return std::abs(quotient - nearest) <= tolerance;
}

[[nodiscard]] std::optional<std::string>
validate_value(const SettingDefinition &definition, const SettingValue &value) {
  const auto invalid = [](std::string message) {
    return std::optional<std::string>(std::move(message));
  };

  switch (definition.kind) {
  case SettingKind::boolean:
    if (!std::holds_alternative<bool>(value))
      return invalid("a boolean value is required");
    return std::nullopt;
  case SettingKind::integer: {
    const auto *number = std::get_if<std::int64_t>(&value);
    const auto *constraint =
        std::get_if<IntegerConstraint>(&definition.constraint);
    if (!number)
      return invalid("a signed 64-bit integer value is required");
    if (!constraint || *number < constraint->minimum ||
        *number > constraint->maximum ||
        !integer_aligned(*number, *constraint)) {
      return invalid("integer value is outside its range or step grid");
    }
    return std::nullopt;
  }
  case SettingKind::floating: {
    const auto *number = std::get_if<double>(&value);
    const auto *constraint =
        std::get_if<FloatConstraint>(&definition.constraint);
    if (!number || !std::isfinite(*number))
      return invalid("a finite floating-point value is required");
    if (!constraint || *number < constraint->minimum ||
        *number > constraint->maximum ||
        !floating_aligned(*number, *constraint)) {
      return invalid("floating-point value is outside its range or step grid");
    }
    return std::nullopt;
  }
  case SettingKind::enumeration: {
    const auto *choice = std::get_if<std::string>(&value);
    const auto *constraint =
        std::get_if<EnumConstraint>(&definition.constraint);
    if (!choice)
      return invalid("an enum choice ID is required");
    if (!constraint ||
        std::ranges::none_of(constraint->choices, [&](const EnumChoice &entry) {
          return entry.id == *choice;
        })) {
      return invalid("enum choice ID is not declared by the schema");
    }
    return std::nullopt;
  }
  case SettingKind::text: {
    const auto *text = std::get_if<std::string>(&value);
    const auto *constraint =
        std::get_if<TextConstraint>(&definition.constraint);
    if (!text)
      return invalid("a UTF-8 text value is required");
    const auto points = utf8_code_points(*text);
    if (!points)
      return invalid("text is not valid UTF-8");
    if (!constraint || *points < constraint->minimum_code_points ||
        *points > constraint->maximum_code_points) {
      return invalid("text length is outside its code-point bounds");
    }
    return std::nullopt;
  }
  case SettingKind::action:
    if (!std::holds_alternative<std::monostate>(value))
      return invalid("actions cannot carry persistent values");
    return std::nullopt;
  case SettingKind::read_only:
    if (std::holds_alternative<std::monostate>(definition.default_value) ||
        value.index() != definition.default_value.index()) {
      return invalid("read-only value type differs from its schema default");
    }
    if (const auto *text = std::get_if<std::string>(&value)) {
      const auto points = utf8_code_points(*text);
      if (!points)
        return invalid("read-only text is not valid UTF-8");
      if (const auto *constraint =
              std::get_if<TextConstraint>(&definition.constraint);
          constraint && (*points < constraint->minimum_code_points ||
                         *points > constraint->maximum_code_points)) {
        return invalid("read-only text length is outside its bounds");
      }
    }
    if (const auto *number = std::get_if<std::int64_t>(&value)) {
      if (const auto *constraint =
              std::get_if<IntegerConstraint>(&definition.constraint);
          constraint &&
          (*number < constraint->minimum || *number > constraint->maximum ||
           !integer_aligned(*number, *constraint))) {
        return invalid("read-only integer is outside its range or step grid");
      }
    }
    if (const auto *number = std::get_if<double>(&value)) {
      if (!std::isfinite(*number))
        return invalid("read-only floating-point value must be finite");
      if (const auto *constraint =
              std::get_if<FloatConstraint>(&definition.constraint);
          constraint &&
          (*number < constraint->minimum || *number > constraint->maximum ||
           !floating_aligned(*number, *constraint))) {
        return invalid(
            "read-only floating-point value is outside its range or step grid");
      }
    }
    return std::nullopt;
  }
  return invalid("unknown setting kind");
}

[[nodiscard]] std::optional<std::string>
validate_constraint(const SettingDefinition &definition) {
  switch (definition.kind) {
  case SettingKind::boolean:
    if (!std::holds_alternative<std::monostate>(definition.constraint))
      return "boolean settings cannot declare a value constraint";
    break;
  case SettingKind::integer: {
    const auto *constraint =
        std::get_if<IntegerConstraint>(&definition.constraint);
    if (!constraint || constraint->minimum > constraint->maximum ||
        constraint->step <= 0)
      return "integer settings require an ordered range and positive step";
    break;
  }
  case SettingKind::floating: {
    const auto *constraint =
        std::get_if<FloatConstraint>(&definition.constraint);
    if (!constraint || !std::isfinite(constraint->minimum) ||
        !std::isfinite(constraint->maximum) ||
        !std::isfinite(constraint->step) ||
        constraint->minimum > constraint->maximum || constraint->step <= 0.0)
      return "floating settings require a finite ordered range and positive "
             "step";
    break;
  }
  case SettingKind::enumeration: {
    const auto *constraint =
        std::get_if<EnumConstraint>(&definition.constraint);
    if (!constraint || constraint->choices.empty())
      return "enum settings require at least one choice";
    std::unordered_set<std::string> choices;
    for (const auto &choice : constraint->choices) {
      if (!is_semantic_id(choice.id) || choice.label_key.empty())
        return "enum choices require stable IDs and label keys";
      if (!choices.emplace(choice.id).second)
        return "enum choice IDs must be unique";
    }
    break;
  }
  case SettingKind::text: {
    const auto *constraint =
        std::get_if<TextConstraint>(&definition.constraint);
    if (!constraint ||
        constraint->minimum_code_points > constraint->maximum_code_points)
      return "text settings require ordered code-point bounds";
    break;
  }
  case SettingKind::action:
    if (!std::holds_alternative<std::monostate>(definition.constraint))
      return "actions cannot declare a value constraint";
    break;
  case SettingKind::read_only: {
    const bool valid_constraint =
        std::holds_alternative<std::monostate>(definition.constraint) ||
        (std::holds_alternative<std::int64_t>(definition.default_value) &&
         std::holds_alternative<IntegerConstraint>(definition.constraint)) ||
        (std::holds_alternative<double>(definition.default_value) &&
         std::holds_alternative<FloatConstraint>(definition.constraint)) ||
        (std::holds_alternative<std::string>(definition.default_value) &&
         std::holds_alternative<TextConstraint>(definition.constraint));
    if (!valid_constraint)
      return "read-only constraints must match the declared value type";
    if (const auto *integer =
            std::get_if<IntegerConstraint>(&definition.constraint);
        integer && (integer->minimum > integer->maximum || integer->step <= 0))
      return "read-only integer constraint is invalid";
    if (const auto *floating =
            std::get_if<FloatConstraint>(&definition.constraint);
        floating &&
        (!std::isfinite(floating->minimum) ||
         !std::isfinite(floating->maximum) || !std::isfinite(floating->step) ||
         floating->minimum > floating->maximum || floating->step <= 0.0))
      return "read-only floating constraint is invalid";
    if (const auto *text = std::get_if<TextConstraint>(&definition.constraint);
        text && text->minimum_code_points > text->maximum_code_points)
      return "read-only text constraint is invalid";
    break;
  }
  }
  return std::nullopt;
}

[[nodiscard]] bool is_mutable_value_kind(SettingKind kind) noexcept {
  return kind != SettingKind::action && kind != SettingKind::read_only;
}

} // namespace

std::string_view kind_token(SettingKind kind) noexcept {
  switch (kind) {
  case SettingKind::boolean:
    return "boolean";
  case SettingKind::integer:
    return "int64";
  case SettingKind::floating:
    return "float64";
  case SettingKind::enumeration:
    return "enum";
  case SettingKind::text:
    return "text";
  case SettingKind::action:
    return "action";
  case SettingKind::read_only:
    return "read-only";
  }
  return {};
}

std::optional<SettingKind>
setting_kind_from_token(std::string_view token) noexcept {
  if (token == "boolean")
    return SettingKind::boolean;
  if (token == "int64")
    return SettingKind::integer;
  if (token == "float64")
    return SettingKind::floating;
  if (token == "enum")
    return SettingKind::enumeration;
  if (token == "text")
    return SettingKind::text;
  if (token == "action")
    return SettingKind::action;
  if (token == "read-only")
    return SettingKind::read_only;
  return std::nullopt;
}

SettingsSchema::SettingsSchema(std::vector<SettingDefinition> definitions,
                               IndexMap indices)
    : definitions_(std::move(definitions)), indices_(std::move(indices)) {}

SettingsResult<std::shared_ptr<const SettingsSchema>>
SettingsSchema::create(std::vector<SettingDefinition> definitions) {
  if (definitions.empty()) {
    return SettingsResult<std::shared_ptr<const SettingsSchema>>::failure(
        error(SettingsErrorCode::invalid_schema, {},
              "settings schema must contain at least one definition"));
  }

  IndexMap indices;
  indices.reserve(definitions.size());
  for (std::size_t index = 0; index < definitions.size(); ++index) {
    const auto &definition = definitions[index];
    if (!is_semantic_id(definition.id)) {
      return SettingsResult<std::shared_ptr<const SettingsSchema>>::failure(
          error(SettingsErrorCode::invalid_id, definition.id,
                "setting ID must use lowercase semantic-ID syntax"));
    }
    if (!indices.emplace(definition.id, index).second) {
      return SettingsResult<std::shared_ptr<const SettingsSchema>>::failure(
          error(SettingsErrorCode::duplicate_id, definition.id,
                "setting IDs must be unique"));
    }
    if (definition.label_key.empty()) {
      return SettingsResult<std::shared_ptr<const SettingsSchema>>::failure(
          error(SettingsErrorCode::invalid_schema, definition.id,
                "every setting requires a localization label key"));
    }
    if (const auto constraint_error = validate_constraint(definition)) {
      return SettingsResult<std::shared_ptr<const SettingsSchema>>::failure(
          error(SettingsErrorCode::invalid_constraint, definition.id,
                *constraint_error));
    }
    if (const auto default_error =
            validate_value(definition, definition.default_value)) {
      return SettingsResult<std::shared_ptr<const SettingsSchema>>::failure(
          error(SettingsErrorCode::invalid_default, definition.id,
                *default_error));
    }
    if (is_mutable_value_kind(definition.kind)) {
      if (!is_semantic_id(definition.reset_scope)) {
        return SettingsResult<std::shared_ptr<const SettingsSchema>>::failure(
            error(SettingsErrorCode::invalid_scope, definition.id,
                  "mutable settings require a stable reset scope"));
      }
    } else if (!definition.reset_scope.empty()) {
      return SettingsResult<std::shared_ptr<const SettingsSchema>>::failure(
          error(SettingsErrorCode::invalid_scope, definition.id,
                "actions and read-only values cannot belong to reset scopes"));
    }
    if ((definition.kind == SettingKind::action ||
         definition.kind == SettingKind::read_only) &&
        definition.restart_required) {
      return SettingsResult<std::shared_ptr<const SettingsSchema>>::failure(
          error(SettingsErrorCode::invalid_schema, definition.id,
                "actions and read-only values cannot require restart"));
    }
    if (definition.visibility.mode == PredicateMode::any &&
        definition.visibility.conditions.empty()) {
      return SettingsResult<std::shared_ptr<const SettingsSchema>>::failure(
          error(SettingsErrorCode::invalid_schema, definition.id,
                "an any-of visibility predicate cannot be empty"));
    }
    if (definition.enabled.mode == PredicateMode::any &&
        definition.enabled.conditions.empty()) {
      return SettingsResult<std::shared_ptr<const SettingsSchema>>::failure(
          error(SettingsErrorCode::invalid_schema, definition.id,
                "an any-of enabled predicate cannot be empty"));
    }
  }

  for (const auto &definition : definitions) {
    const Predicate *predicates[]{&definition.visibility, &definition.enabled};
    for (const auto *predicate : predicates) {
      for (const auto &condition : predicate->conditions) {
        const auto dependency = indices.find(condition.setting_id);
        if (dependency == indices.end()) {
          return SettingsResult<std::shared_ptr<const SettingsSchema>>::failure(
              error(SettingsErrorCode::missing_dependency, definition.id,
                    "predicate references missing setting: " +
                        condition.setting_id));
        }
        const auto &target = definitions[dependency->second];
        if (target.kind == SettingKind::action) {
          return SettingsResult<std::shared_ptr<const SettingsSchema>>::failure(
              error(SettingsErrorCode::invalid_schema, definition.id,
                    "predicates cannot compare action settings"));
        }
        if (const auto expected_error =
                validate_value(target, condition.expected)) {
          return SettingsResult<std::shared_ptr<const SettingsSchema>>::failure(
              error(SettingsErrorCode::invalid_schema, definition.id,
                    "predicate comparison is invalid for " + target.id + ": " +
                        *expected_error));
        }
      }
    }
  }

  enum class Visit : unsigned char { unseen, active, complete };
  std::vector<Visit> visits(definitions.size(), Visit::unseen);
  std::function<std::optional<std::string>(std::size_t)> visit =
      [&](std::size_t index) -> std::optional<std::string> {
    if (visits[index] == Visit::active)
      return definitions[index].id;
    if (visits[index] == Visit::complete)
      return std::nullopt;
    visits[index] = Visit::active;
    const Predicate *predicates[]{&definitions[index].visibility,
                                  &definitions[index].enabled};
    for (const auto *predicate : predicates) {
      for (const auto &condition : predicate->conditions) {
        const auto target = indices.find(condition.setting_id);
        if (const auto cycle = visit(target->second))
          return cycle;
      }
    }
    visits[index] = Visit::complete;
    return std::nullopt;
  };
  for (std::size_t index = 0; index < definitions.size(); ++index) {
    if (const auto cycle = visit(index)) {
      return SettingsResult<std::shared_ptr<const SettingsSchema>>::failure(
          error(SettingsErrorCode::dependency_cycle, *cycle,
                "visibility/enabled dependency graph contains a cycle"));
    }
  }

  auto schema = std::shared_ptr<const SettingsSchema>(
      new SettingsSchema(std::move(definitions), std::move(indices)));
  return SettingsResult<std::shared_ptr<const SettingsSchema>>::success(
      std::move(schema));
}

std::span<const SettingDefinition>
SettingsSchema::definitions() const noexcept {
  return definitions_;
}

const SettingDefinition *
SettingsSchema::find(std::string_view id) const noexcept {
  const auto index = index_of(id);
  return index ? &definitions_[*index] : nullptr;
}

std::optional<std::size_t>
SettingsSchema::index_of(std::string_view id) const noexcept {
  const auto found = indices_.find(id);
  if (found == indices_.end())
    return std::nullopt;
  return found->second;
}

SettingsTransaction &SettingsTransaction::set(std::string setting_id,
                                              SettingValue value) {
  mutations_.push_back(
      {MutationOperation::set, std::move(setting_id), std::move(value)});
  return *this;
}

SettingsTransaction &SettingsTransaction::reset(std::string setting_id) {
  mutations_.push_back(
      {MutationOperation::reset, std::move(setting_id), std::monostate{}});
  return *this;
}

SettingsTransaction &SettingsTransaction::invoke(std::string setting_id) {
  mutations_.push_back(
      {MutationOperation::invoke, std::move(setting_id), std::monostate{}});
  return *this;
}

std::span<const Mutation> SettingsTransaction::mutations() const noexcept {
  return mutations_;
}

bool SettingsTransaction::empty() const noexcept { return mutations_.empty(); }

const SettingView *SettingsSnapshot::find(std::string_view id) const noexcept {
  const auto found = std::ranges::find(settings, id, &SettingView::id);
  return found == settings.end() ? nullptr : &*found;
}

SettingsState::SettingsState(std::shared_ptr<const SettingsSchema> schema,
                             std::vector<SettingValue> values)
    : schema_(std::move(schema)), values_(std::move(values)),
      saved_values_(values_) {}

SettingsResult<SettingsState>
SettingsState::create(std::shared_ptr<const SettingsSchema> schema) {
  if (!schema) {
    return SettingsResult<SettingsState>::failure(
        error(SettingsErrorCode::null_schema, {},
              "settings state requires an immutable schema"));
  }
  std::vector<SettingValue> values;
  values.reserve(schema->definitions().size());
  for (const auto &definition : schema->definitions())
    values.push_back(definition.default_value);
  return SettingsResult<SettingsState>::success(
      SettingsState(std::move(schema), std::move(values)));
}

const std::shared_ptr<const SettingsSchema> &
SettingsState::schema() const noexcept {
  return schema_;
}

SettingsResult<SettingValue>
SettingsState::value(std::string_view setting_id) const {
  const auto index = schema_->index_of(setting_id);
  if (!index) {
    return SettingsResult<SettingValue>::failure(
        error(SettingsErrorCode::unknown_setting, setting_id,
              "setting ID is not declared by the schema"));
  }
  return SettingsResult<SettingValue>::success(values_[*index]);
}

bool SettingsState::evaluate(const Predicate &predicate,
                             std::span<const SettingValue> values) const {
  if (predicate.conditions.empty())
    return true;
  const auto matches = [&](const PredicateCondition &condition) {
    const auto index = schema_->index_of(condition.setting_id);
    if (!index || *index >= values.size())
      return false;
    const bool equal = values[*index] == condition.expected;
    return condition.comparison == CompareOperator::equal ? equal : !equal;
  };
  if (predicate.mode == PredicateMode::all)
    return std::ranges::all_of(predicate.conditions, matches);
  return std::ranges::any_of(predicate.conditions, matches);
}

bool SettingsState::dirty() const noexcept {
  const auto definitions = schema_->definitions();
  for (std::size_t index = 0; index < definitions.size(); ++index) {
    if (is_mutable_value_kind(definitions[index].kind) &&
        values_[index] != saved_values_[index])
      return true;
  }
  return false;
}

bool SettingsState::restart_pending() const noexcept {
  const auto definitions = schema_->definitions();
  for (std::size_t index = 0; index < definitions.size(); ++index) {
    if (definitions[index].restart_required &&
        values_[index] != saved_values_[index])
      return true;
  }
  return false;
}

CommitReport
SettingsState::make_report(std::vector<std::string> changed_ids,
                           std::vector<std::string> invoked_action_ids) const {
  return {revision_, std::move(changed_ids), std::move(invoked_action_ids),
          dirty(), restart_pending()};
}

SettingsSnapshot SettingsState::snapshot() const {
  SettingsSnapshot result;
  result.revision = revision_;
  result.dirty = dirty();
  result.restart_pending = restart_pending();
  const auto definitions = schema_->definitions();
  result.settings.reserve(definitions.size());
  result.records.reserve(definitions.size());
  for (std::size_t index = 0; index < definitions.size(); ++index) {
    const auto &definition = definitions[index];
    result.settings.push_back({definition.id, definition.kind, values_[index],
                               evaluate(definition.visibility, values_),
                               evaluate(definition.enabled, values_),
                               is_mutable_value_kind(definition.kind) &&
                                   values_[index] != saved_values_[index],
                               definition.restart_required});
    if (definition.kind != SettingKind::action) {
      result.records.push_back({value_record_format_version, definition.id,
                                definition.kind, values_[index]});
    }
  }
  return result;
}

SettingsResult<CommitReport>
SettingsState::apply(const SettingsTransaction &transaction,
                     MutationAuthority authority) {
  std::vector<SettingValue> prospective = values_;
  std::vector<std::string> changed_ids;
  std::vector<std::string> invoked_ids;
  changed_ids.reserve(transaction.mutations().size());
  invoked_ids.reserve(transaction.mutations().size());
  std::unordered_set<std::string> touched;
  touched.reserve(transaction.mutations().size());

  struct Target {
    std::size_t index{};
    MutationOperation operation{MutationOperation::set};
  };
  std::vector<Target> targets;
  targets.reserve(transaction.mutations().size());

  for (const auto &mutation : transaction.mutations()) {
    const auto index = schema_->index_of(mutation.setting_id);
    if (!index) {
      return SettingsResult<CommitReport>::failure(
          error(SettingsErrorCode::unknown_setting, mutation.setting_id,
                "transaction references an unknown setting"));
    }
    if (!touched.emplace(mutation.setting_id).second) {
      return SettingsResult<CommitReport>::failure(
          error(SettingsErrorCode::duplicate_mutation, mutation.setting_id,
                "a transaction may target each setting at most once"));
    }
    const auto &definition = schema_->definitions()[*index];
    switch (mutation.operation) {
    case MutationOperation::set:
      if (definition.kind == SettingKind::action) {
        return SettingsResult<CommitReport>::failure(
            error(SettingsErrorCode::not_action, mutation.setting_id,
                  "actions must be invoked, not assigned"));
      }
      if (definition.kind == SettingKind::read_only &&
          authority != MutationAuthority::backend) {
        return SettingsResult<CommitReport>::failure(
            error(SettingsErrorCode::read_only, mutation.setting_id,
                  "read-only values may only be published by a backend"));
      }
      if (const auto value_error = validate_value(definition, mutation.value)) {
        return SettingsResult<CommitReport>::failure(
            error(SettingsErrorCode::invalid_value, mutation.setting_id,
                  *value_error));
      }
      prospective[*index] = mutation.value;
      break;
    case MutationOperation::reset:
      if (definition.kind == SettingKind::action) {
        return SettingsResult<CommitReport>::failure(
            error(SettingsErrorCode::not_action, mutation.setting_id,
                  "actions do not have resettable values"));
      }
      if (definition.kind == SettingKind::read_only) {
        return SettingsResult<CommitReport>::failure(
            error(SettingsErrorCode::read_only, mutation.setting_id,
                  "read-only values cannot be reset"));
      }
      prospective[*index] = definition.default_value;
      break;
    case MutationOperation::invoke:
      if (definition.kind != SettingKind::action) {
        return SettingsResult<CommitReport>::failure(
            error(SettingsErrorCode::not_action, mutation.setting_id,
                  "only action settings can be invoked"));
      }
      break;
    }
    targets.push_back({*index, mutation.operation});
  }

  if (authority == MutationAuthority::user) {
    for (const auto &target : targets) {
      const auto &definition = schema_->definitions()[target.index];
      if (!evaluate(definition.visibility, prospective)) {
        return SettingsResult<CommitReport>::failure(
            error(SettingsErrorCode::hidden, definition.id,
                  "user mutation targets a hidden setting"));
      }
      if (!evaluate(definition.enabled, prospective)) {
        return SettingsResult<CommitReport>::failure(
            error(SettingsErrorCode::disabled, definition.id,
                  "user mutation targets a disabled setting"));
      }
    }
  }

  for (const auto &target : targets) {
    const auto &definition = schema_->definitions()[target.index];
    if (target.operation == MutationOperation::invoke) {
      invoked_ids.push_back(definition.id);
    } else if (prospective[target.index] != values_[target.index]) {
      changed_ids.push_back(definition.id);
    }
  }
  if (!changed_ids.empty() || !invoked_ids.empty()) {
    values_ = std::move(prospective);
    if (revision_ != std::numeric_limits<std::uint64_t>::max())
      ++revision_;
  }
  return SettingsResult<CommitReport>::success(
      make_report(std::move(changed_ids), std::move(invoked_ids)));
}

SettingsResult<CommitReport>
SettingsState::reset_scope(std::string_view scope) {
  if (!is_semantic_id(scope)) {
    return SettingsResult<CommitReport>::failure(
        error(SettingsErrorCode::invalid_scope, scope,
              "reset scope must use semantic-ID syntax"));
  }
  SettingsTransaction transaction;
  for (const auto &definition : schema_->definitions()) {
    if (definition.reset_scope == scope)
      transaction.reset(definition.id);
  }
  if (transaction.empty()) {
    return SettingsResult<CommitReport>::failure(
        error(SettingsErrorCode::invalid_scope, scope,
              "reset scope is not declared by the schema"));
  }
  return apply(transaction, MutationAuthority::backend);
}

SettingsResult<CommitReport> SettingsState::reset_all() {
  SettingsTransaction transaction;
  for (const auto &definition : schema_->definitions()) {
    if (is_mutable_value_kind(definition.kind))
      transaction.reset(definition.id);
  }
  return apply(transaction, MutationAuthority::backend);
}

SettingsResult<RestoreReport>
SettingsState::restore(std::span<const ValueRecord> records) {
  std::vector<SettingValue> prospective;
  prospective.reserve(schema_->definitions().size());
  for (const auto &definition : schema_->definitions())
    prospective.push_back(definition.default_value);

  std::unordered_set<std::string> restored;
  restored.reserve(records.size());
  std::vector<RestoreDiagnostic> diagnostics;
  std::size_t restored_count = 0;
  for (const auto &record : records) {
    const auto index = schema_->index_of(record.setting_id);
    if (!index) {
      diagnostics.push_back(
          {RestoreDiagnosticCode::unknown_setting, record.setting_id,
           "record was preserved as a diagnostic and ignored"});
      continue;
    }
    if (!restored.emplace(record.setting_id).second) {
      return SettingsResult<RestoreReport>::failure(
          error(SettingsErrorCode::duplicate_record, record.setting_id,
                "restore input contains a duplicate known setting"));
    }
    const auto &definition = schema_->definitions()[*index];
    if (record.format_version != value_record_format_version) {
      return SettingsResult<RestoreReport>::failure(error(
          SettingsErrorCode::unsupported_record_version, record.setting_id,
          "known setting uses an unsupported value-record version"));
    }
    if (definition.kind == SettingKind::action) {
      return SettingsResult<RestoreReport>::failure(
          error(SettingsErrorCode::not_action, record.setting_id,
                "actions must not appear in persisted value records"));
    }
    if (record.kind != definition.kind) {
      return SettingsResult<RestoreReport>::failure(
          error(SettingsErrorCode::kind_mismatch, record.setting_id,
                "record kind differs from the immutable schema"));
    }
    if (const auto value_error = validate_value(definition, record.value)) {
      return SettingsResult<RestoreReport>::failure(error(
          SettingsErrorCode::invalid_value, record.setting_id, *value_error));
    }
    prospective[*index] = record.value;
    ++restored_count;
  }

  if (values_ != prospective || saved_values_ != prospective) {
    values_ = prospective;
    saved_values_ = std::move(prospective);
    if (revision_ != std::numeric_limits<std::uint64_t>::max())
      ++revision_;
  }
  return SettingsResult<RestoreReport>::success(
      {revision_, restored_count, std::move(diagnostics)});
}

CommitReport SettingsState::mark_saved() {
  if (saved_values_ != values_) {
    saved_values_ = values_;
    if (revision_ != std::numeric_limits<std::uint64_t>::max())
      ++revision_;
  }
  return make_report({}, {});
}

} // namespace openxmb::settings
