#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace openxmb::settings {

enum class SettingKind {
  boolean,
  integer,
  floating,
  enumeration,
  text,
  action,
  read_only,
};

// Stable wire tokens for persistence backends. Enum ordinals are deliberately
// not a serialization contract.
[[nodiscard]] std::string_view kind_token(SettingKind kind) noexcept;
[[nodiscard]] std::optional<SettingKind>
setting_kind_from_token(std::string_view token) noexcept;

using SettingValue =
    std::variant<std::monostate, bool, std::int64_t, double, std::string>;

struct IntegerConstraint {
  std::int64_t minimum{};
  std::int64_t maximum{};
  std::int64_t step{1};

  [[nodiscard]] bool operator==(const IntegerConstraint &) const = default;
};

struct FloatConstraint {
  double minimum{};
  double maximum{};
  double step{};

  [[nodiscard]] bool operator==(const FloatConstraint &) const = default;
};

struct TextConstraint {
  std::size_t minimum_code_points{};
  std::size_t maximum_code_points{4096};

  [[nodiscard]] bool operator==(const TextConstraint &) const = default;
};

struct EnumChoice {
  std::string id;
  std::string label_key;

  [[nodiscard]] bool operator==(const EnumChoice &) const = default;
};

struct EnumConstraint {
  std::vector<EnumChoice> choices;

  [[nodiscard]] bool operator==(const EnumConstraint &) const = default;
};

using ValueConstraint =
    std::variant<std::monostate, IntegerConstraint, FloatConstraint,
                 TextConstraint, EnumConstraint>;

enum class CompareOperator { equal, not_equal };

struct PredicateCondition {
  std::string setting_id;
  CompareOperator comparison{CompareOperator::equal};
  SettingValue expected;

  [[nodiscard]] bool operator==(const PredicateCondition &) const = default;
};

enum class PredicateMode { all, any };

struct Predicate {
  PredicateMode mode{PredicateMode::all};
  std::vector<PredicateCondition> conditions;

  [[nodiscard]] bool operator==(const Predicate &) const = default;
};

struct SettingDefinition {
  std::string id;
  std::string label_key;
  std::string description_key;
  SettingKind kind{SettingKind::boolean};
  SettingValue default_value;
  ValueConstraint constraint;
  std::string reset_scope;
  bool restart_required{};
  Predicate visibility;
  Predicate enabled;

  [[nodiscard]] bool operator==(const SettingDefinition &) const = default;
};

enum class SettingsErrorCode {
  invalid_schema,
  invalid_id,
  duplicate_id,
  invalid_constraint,
  invalid_default,
  missing_dependency,
  dependency_cycle,
  null_schema,
  unknown_setting,
  duplicate_mutation,
  duplicate_record,
  unsupported_record_version,
  kind_mismatch,
  invalid_value,
  read_only,
  not_action,
  hidden,
  disabled,
  invalid_scope,
};

struct SettingsError {
  SettingsErrorCode code{SettingsErrorCode::invalid_schema};
  std::string subject_id;
  std::string detail;

  [[nodiscard]] bool operator==(const SettingsError &) const = default;
};

template <typename T> class [[nodiscard]] SettingsResult {
public:
  static SettingsResult success(T value) {
    return SettingsResult(std::in_place_index<0>, std::move(value));
  }

  static SettingsResult failure(SettingsError error) {
    return SettingsResult(std::in_place_index<1>, std::move(error));
  }

  [[nodiscard]] bool has_value() const noexcept {
    return storage_.index() == 0;
  }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T &value() & { return std::get<0>(storage_); }
  [[nodiscard]] const T &value() const & { return std::get<0>(storage_); }
  [[nodiscard]] T &&value() && { return std::get<0>(std::move(storage_)); }
  [[nodiscard]] SettingsError &error() & { return std::get<1>(storage_); }
  [[nodiscard]] const SettingsError &error() const & {
    return std::get<1>(storage_);
  }

private:
  template <std::size_t Index, typename Value>
  explicit SettingsResult(std::in_place_index_t<Index> tag, Value &&value)
      : storage_(tag, std::forward<Value>(value)) {}

  std::variant<T, SettingsError> storage_;
};

class SettingsSchema final {
public:
  [[nodiscard]] static SettingsResult<std::shared_ptr<const SettingsSchema>>
  create(std::vector<SettingDefinition> definitions);

  [[nodiscard]] std::span<const SettingDefinition> definitions() const noexcept;
  [[nodiscard]] const SettingDefinition *
  find(std::string_view id) const noexcept;
  [[nodiscard]] std::optional<std::size_t>
  index_of(std::string_view id) const noexcept;

private:
  struct IdHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t
    operator()(std::string_view value) const noexcept {
      return std::hash<std::string_view>{}(value);
    }
  };

  struct IdEqual {
    using is_transparent = void;
    [[nodiscard]] bool operator()(std::string_view left,
                                  std::string_view right) const noexcept {
      return left == right;
    }
  };

  using IndexMap =
      std::unordered_map<std::string, std::size_t, IdHash, IdEqual>;

  explicit SettingsSchema(std::vector<SettingDefinition> definitions,
                          IndexMap indices);

  std::vector<SettingDefinition> definitions_;
  IndexMap indices_;
};

enum class MutationAuthority { user, backend };
enum class MutationOperation { set, reset, invoke };

struct Mutation {
  MutationOperation operation{MutationOperation::set};
  std::string setting_id;
  SettingValue value;
};

class SettingsTransaction final {
public:
  SettingsTransaction &set(std::string setting_id, SettingValue value);
  SettingsTransaction &reset(std::string setting_id);
  SettingsTransaction &invoke(std::string setting_id);

  [[nodiscard]] std::span<const Mutation> mutations() const noexcept;
  [[nodiscard]] bool empty() const noexcept;

private:
  std::vector<Mutation> mutations_;
};

struct CommitReport {
  std::uint64_t revision{};
  std::vector<std::string> changed_ids;
  std::vector<std::string> invoked_action_ids;
  bool dirty{};
  bool restart_pending{};

  [[nodiscard]] bool operator==(const CommitReport &) const = default;
};

inline constexpr std::uint32_t value_record_format_version = 1;

struct ValueRecord {
  std::uint32_t format_version{value_record_format_version};
  std::string setting_id;
  SettingKind kind{SettingKind::boolean};
  SettingValue value;

  [[nodiscard]] bool operator==(const ValueRecord &) const = default;
};

enum class RestoreDiagnosticCode { unknown_setting };

struct RestoreDiagnostic {
  RestoreDiagnosticCode code{RestoreDiagnosticCode::unknown_setting};
  std::string setting_id;
  std::string detail;

  [[nodiscard]] bool operator==(const RestoreDiagnostic &) const = default;
};

struct RestoreReport {
  std::uint64_t revision{};
  std::size_t restored_count{};
  std::vector<RestoreDiagnostic> diagnostics;

  [[nodiscard]] bool operator==(const RestoreReport &) const = default;
};

struct SettingView {
  std::string id;
  SettingKind kind{SettingKind::boolean};
  SettingValue value;
  bool visible{true};
  bool enabled{true};
  bool dirty{};
  bool restart_required{};

  [[nodiscard]] bool operator==(const SettingView &) const = default;
};

// This is a deep copy and may be safely handed to another thread. The mutable
// SettingsState that produced it remains intentionally thread-confined.
struct SettingsSnapshot {
  std::uint64_t revision{};
  std::vector<SettingView> settings;
  std::vector<ValueRecord> records;
  bool dirty{};
  bool restart_pending{};

  [[nodiscard]] const SettingView *find(std::string_view id) const noexcept;
};

class SettingsState final {
public:
  [[nodiscard]] static SettingsResult<SettingsState>
  create(std::shared_ptr<const SettingsSchema> schema);

  [[nodiscard]] const std::shared_ptr<const SettingsSchema> &
  schema() const noexcept;
  [[nodiscard]] SettingsResult<SettingValue>
  value(std::string_view setting_id) const;
  [[nodiscard]] SettingsSnapshot snapshot() const;

  [[nodiscard]] SettingsResult<CommitReport>
  apply(const SettingsTransaction &transaction,
        MutationAuthority authority = MutationAuthority::user);
  [[nodiscard]] SettingsResult<CommitReport>
  reset_scope(std::string_view scope);
  [[nodiscard]] SettingsResult<CommitReport> reset_all();
  [[nodiscard]] SettingsResult<RestoreReport>
  restore(std::span<const ValueRecord> records);

  // Marks the current values as the successfully persisted baseline.
  [[nodiscard]] CommitReport mark_saved();

private:
  SettingsState(std::shared_ptr<const SettingsSchema> schema,
                std::vector<SettingValue> values);

  [[nodiscard]] bool evaluate(const Predicate &predicate,
                              std::span<const SettingValue> values) const;
  [[nodiscard]] bool dirty() const noexcept;
  [[nodiscard]] bool restart_pending() const noexcept;
  [[nodiscard]] CommitReport
  make_report(std::vector<std::string> changed_ids,
              std::vector<std::string> invoked_action_ids) const;

  std::shared_ptr<const SettingsSchema> schema_;
  std::vector<SettingValue> values_;
  std::vector<SettingValue> saved_values_;
  std::uint64_t revision_{};
};

} // namespace openxmb::settings
