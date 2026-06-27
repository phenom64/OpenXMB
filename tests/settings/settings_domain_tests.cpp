#include "openxmb/settings/domain.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace openxmb::settings;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

template <typename Result>
void expect_error(const Result &result, SettingsErrorCode code,
                  std::string_view message) {
  expect(!result && result.error().code == code, message);
}

Predicate when(std::string id, SettingValue value,
               CompareOperator comparison = CompareOperator::equal) {
  return {PredicateMode::all, {{std::move(id), comparison, std::move(value)}}};
}

SettingDefinition boolean_setting(std::string id, bool default_value,
                                  std::string scope) {
  const auto label = id + ".label";
  return {std::move(id),
          label,
          {},
          SettingKind::boolean,
          default_value,
          std::monostate{},
          std::move(scope),
          false,
          {},
          {}};
}

SettingDefinition integer_setting(std::string id, std::int64_t default_value,
                                  IntegerConstraint constraint,
                                  std::string scope) {
  const auto label = id + ".label";
  return {std::move(id),
          label,
          {},
          SettingKind::integer,
          default_value,
          constraint,
          std::move(scope),
          false,
          {},
          {}};
}

SettingDefinition floating_setting(std::string id, double default_value,
                                   FloatConstraint constraint,
                                   std::string scope) {
  const auto label = id + ".label";
  return {std::move(id),
          label,
          {},
          SettingKind::floating,
          default_value,
          constraint,
          std::move(scope),
          false,
          {},
          {}};
}

SettingDefinition enum_setting(std::string id, std::string default_value,
                               std::vector<EnumChoice> choices,
                               std::string scope) {
  const auto label = id + ".label";
  return {std::move(id),
          label,
          {},
          SettingKind::enumeration,
          std::move(default_value),
          EnumConstraint{std::move(choices)},
          std::move(scope),
          false,
          {},
          {}};
}

SettingDefinition text_setting(std::string id, std::string default_value,
                               TextConstraint constraint, std::string scope) {
  const auto label = id + ".label";
  return {std::move(id),
          label,
          {},
          SettingKind::text,
          std::move(default_value),
          constraint,
          std::move(scope),
          false,
          {},
          {}};
}

SettingDefinition action_setting(std::string id) {
  const auto label = id + ".label";
  return {std::move(id),
          label,
          {},
          SettingKind::action,
          std::monostate{},
          std::monostate{},
          {},
          false,
          {},
          {}};
}

SettingDefinition read_only_setting(std::string id, SettingValue value,
                                    ValueConstraint constraint = {}) {
  const auto label = id + ".label";
  return {std::move(id),
          label,
          {},
          SettingKind::read_only,
          std::move(value),
          std::move(constraint),
          {},
          false,
          {},
          {}};
}

std::vector<SettingDefinition> reference_definitions() {
  std::vector<SettingDefinition> definitions;
  definitions.push_back(
      enum_setting("settings.theme.preset", "original",
                   {{"original", "settings.theme.preset.original.label"},
                    {"classic", "settings.theme.preset.classic.label"}},
                   "appearance"));
  definitions.push_back(
      boolean_setting("settings.theme.particles", true, "appearance"));
  definitions.back().enabled =
      when("settings.theme.preset", std::string("classic"),
           CompareOperator::not_equal);
  definitions.push_back(integer_setting("settings.input.cursor-speed", 100,
                                        {25, 200, 25}, "input"));
  definitions.push_back(floating_setting("settings.audio.music-volume", 0.8,
                                         {0.0, 1.0, 0.1}, "audio"));
  definitions.push_back(
      text_setting("settings.system.name", "Living Room", {1, 32}, "system"));
  definitions.back().restart_required = true;
  definitions.push_back(
      boolean_setting("settings.network.enabled", false, "network"));
  definitions.push_back(
      enum_setting("settings.network.mode", "ethernet",
                   {{"ethernet", "settings.network.mode.ethernet.label"},
                    {"wireless", "settings.network.mode.wireless.label"}},
                   "network"));
  definitions.back().visibility = when("settings.network.enabled", true);
  definitions.back().enabled = when("settings.network.enabled", true);
  definitions.push_back(
      text_setting("settings.network.ssid", "Home", {1, 32}, "network"));
  definitions.back().visibility = {
      PredicateMode::all,
      {{"settings.network.enabled", CompareOperator::equal, true},
       {"settings.network.mode", CompareOperator::equal,
        std::string("wireless")}}};
  definitions.back().enabled = definitions.back().visibility;
  definitions.push_back(action_setting("settings.system.check-update"));
  definitions.push_back(read_only_setting("settings.system.version",
                                          std::string("development"),
                                          TextConstraint{1, 64}));
  return definitions;
}

std::shared_ptr<const SettingsSchema> make_schema() {
  auto result = SettingsSchema::create(reference_definitions());
  if (!result) {
    std::cerr << "schema fixture failed: " << result.error().detail << '\n';
    std::abort();
  }
  return std::move(result).value();
}

SettingsState make_state(const std::shared_ptr<const SettingsSchema> &schema) {
  auto result = SettingsState::create(schema);
  if (!result) {
    std::cerr << "state fixture failed: " << result.error().detail << '\n';
    std::abort();
  }
  return std::move(result).value();
}

void test_schema_and_deterministic_snapshot() {
  const auto schema = make_schema();
  expect(schema->definitions().size() == 10,
         "schema retains every typed definition");
  expect(schema->definitions()[0].id == "settings.theme.preset" &&
             schema->definitions()[9].id == "settings.system.version",
         "schema iteration preserves deterministic declaration order");
  expect(schema->find("settings.audio.music-volume") != nullptr &&
             schema->find("settings.missing") == nullptr,
         "schema lookup uses stable semantic IDs");

  const auto state = make_state(schema);
  const auto snapshot = state.snapshot();
  expect(snapshot.revision == 0 && !snapshot.dirty && !snapshot.restart_pending,
         "default snapshot is clean at revision zero");
  expect(snapshot.settings.size() == 10 && snapshot.records.size() == 9,
         "snapshot includes actions as views but excludes them from records");
  expect(snapshot.settings[0].id == "settings.theme.preset" &&
             snapshot.records[0].setting_id == "settings.theme.preset" &&
             snapshot.records.back().setting_id == "settings.system.version",
         "views and serial records preserve schema order");
  expect(snapshot.find("settings.network.mode") != nullptr &&
             !snapshot.find("settings.network.mode")->visible &&
             !snapshot.find("settings.network.mode")->enabled,
         "dependency predicates are reflected in immutable views");
  const SettingKind kinds[]{
      SettingKind::boolean,     SettingKind::integer, SettingKind::floating,
      SettingKind::enumeration, SettingKind::text,    SettingKind::action,
      SettingKind::read_only,
  };
  for (const auto kind : kinds) {
    expect(setting_kind_from_token(kind_token(kind)) == kind,
           "stable backend-neutral kind tokens round-trip");
  }
  expect(!setting_kind_from_token("future-kind"),
         "unknown wire kind tokens are forward-compatible parse misses");
}

void test_invalid_schemas() {
  expect_error(SettingsSchema::create({}), SettingsErrorCode::invalid_schema,
               "empty schemas are rejected");

  auto bad_id = reference_definitions();
  bad_id[0].id = "Settings.Theme";
  expect_error(SettingsSchema::create(std::move(bad_id)),
               SettingsErrorCode::invalid_id, "non-semantic IDs are rejected");

  auto duplicate = reference_definitions();
  duplicate[1].id = duplicate[0].id;
  expect_error(SettingsSchema::create(std::move(duplicate)),
               SettingsErrorCode::duplicate_id,
               "duplicate setting IDs are rejected");

  auto bad_range = reference_definitions();
  bad_range[2].constraint = IntegerConstraint{200, 25, 0};
  expect_error(SettingsSchema::create(std::move(bad_range)),
               SettingsErrorCode::invalid_constraint,
               "invalid integer bounds and step are rejected");

  auto bad_float = reference_definitions();
  bad_float[3].constraint =
      FloatConstraint{0.0, 1.0, std::numeric_limits<double>::quiet_NaN()};
  expect_error(SettingsSchema::create(std::move(bad_float)),
               SettingsErrorCode::invalid_constraint,
               "non-finite floating constraints are rejected");

  auto bad_default = reference_definitions();
  bad_default[2].default_value = std::int64_t{110};
  expect_error(SettingsSchema::create(std::move(bad_default)),
               SettingsErrorCode::invalid_default,
               "off-grid defaults are rejected");

  auto bad_enum = reference_definitions();
  bad_enum[0].default_value = std::string("missing");
  expect_error(SettingsSchema::create(std::move(bad_enum)),
               SettingsErrorCode::invalid_default,
               "unknown enum defaults are rejected");

  auto missing_dependency = reference_definitions();
  missing_dependency[1].enabled = when("settings.does-not-exist", true);
  expect_error(SettingsSchema::create(std::move(missing_dependency)),
               SettingsErrorCode::missing_dependency,
               "missing predicate dependencies are rejected");

  auto mismatched_dependency = reference_definitions();
  mismatched_dependency[1].enabled =
      when("settings.input.cursor-speed", std::string("fast"));
  expect_error(SettingsSchema::create(std::move(mismatched_dependency)),
               SettingsErrorCode::invalid_schema,
               "predicate values must match dependency types");

  auto cycle = reference_definitions();
  cycle[0].enabled = when("settings.theme.particles", true);
  cycle[1].enabled = when("settings.theme.preset", std::string("original"));
  expect_error(SettingsSchema::create(std::move(cycle)),
               SettingsErrorCode::dependency_cycle,
               "multi-node dependency cycles are rejected");

  auto self_cycle = reference_definitions();
  self_cycle[2].visibility =
      when("settings.input.cursor-speed", std::int64_t{100});
  expect_error(SettingsSchema::create(std::move(self_cycle)),
               SettingsErrorCode::dependency_cycle,
               "self dependency cycles are rejected");

  auto empty_any = reference_definitions();
  empty_any[0].visibility.mode = PredicateMode::any;
  expect_error(SettingsSchema::create(std::move(empty_any)),
               SettingsErrorCode::invalid_schema,
               "empty any-of predicates are rejected as ambiguous");
}

void test_atomic_mutation_and_value_validation() {
  const auto schema = make_schema();
  auto state = make_state(schema);
  const auto initial = state.snapshot();

  SettingsTransaction invalid_batch;
  invalid_batch.set("settings.input.cursor-speed", std::int64_t{125})
      .set("settings.audio.music-volume", 0.85);
  expect_error(state.apply(invalid_batch), SettingsErrorCode::invalid_value,
               "invalid trailing mutation rejects a whole transaction");
  expect(state.snapshot().records == initial.records &&
             state.snapshot().revision == initial.revision,
         "failed transaction rolls back values and revision atomically");

  SettingsTransaction wrong_type;
  wrong_type.set("settings.input.cursor-speed", 100.0);
  expect_error(state.apply(wrong_type), SettingsErrorCode::invalid_value,
               "integer settings reject floating values");

  SettingsTransaction out_of_range;
  out_of_range.set("settings.input.cursor-speed", std::int64_t{225});
  expect_error(state.apply(out_of_range), SettingsErrorCode::invalid_value,
               "bounded integers reject out-of-range values");

  SettingsTransaction unknown_enum;
  unknown_enum.set("settings.theme.preset", std::string("future"));
  expect_error(state.apply(unknown_enum), SettingsErrorCode::invalid_value,
               "enums reject undeclared semantic choices");

  SettingsTransaction invalid_text;
  invalid_text.set("settings.system.name", std::string("\xc3\x28", 2));
  expect_error(state.apply(invalid_text), SettingsErrorCode::invalid_value,
               "text settings reject malformed UTF-8");

  SettingsTransaction unknown;
  unknown.set("settings.unknown", true);
  expect_error(state.apply(unknown), SettingsErrorCode::unknown_setting,
               "mutations reject unknown setting IDs");

  SettingsTransaction duplicate;
  duplicate.set("settings.theme.preset", std::string("classic"))
      .reset("settings.theme.preset");
  expect_error(state.apply(duplicate), SettingsErrorCode::duplicate_mutation,
               "transactions reject ambiguous duplicate targets");

  SettingsTransaction assign_action;
  assign_action.set("settings.system.check-update", true);
  expect_error(state.apply(assign_action), SettingsErrorCode::not_action,
               "actions cannot be assigned values");

  SettingsTransaction invoke_value;
  invoke_value.invoke("settings.theme.preset");
  expect_error(state.apply(invoke_value), SettingsErrorCode::not_action,
               "ordinary values cannot be invoked");

  SettingsTransaction update_read_only;
  update_read_only.set("settings.system.version", std::string("1.0"));
  expect_error(state.apply(update_read_only), SettingsErrorCode::read_only,
               "users cannot assign read-only values");
  const auto backend =
      state.apply(update_read_only, MutationAuthority::backend);
  expect(
      backend && !backend.value().dirty &&
          std::get<std::string>(
              state.snapshot().find("settings.system.version")->value) == "1.0",
      "backends can publish typed read-only values without dirtying settings");
}

void test_prospective_dependencies_and_actions() {
  const auto schema = make_schema();
  auto state = make_state(schema);

  SettingsTransaction hidden;
  hidden.set("settings.network.mode", std::string("wireless"));
  expect_error(state.apply(hidden), SettingsErrorCode::hidden,
               "hidden dependent values reject direct user mutation");

  SettingsTransaction enable_and_configure;
  enable_and_configure.set("settings.network.enabled", true)
      .set("settings.network.mode", std::string("wireless"))
      .set("settings.network.ssid", std::string("Caf\xc3\xa9"));
  const auto configured = state.apply(enable_and_configure);
  expect(configured && configured.value().changed_ids.size() == 3,
         "dependencies evaluate against the final prospective transaction");
  const auto configured_snapshot = state.snapshot();
  expect(configured_snapshot.find("settings.network.ssid")->visible &&
             configured_snapshot.find("settings.network.ssid")->enabled,
         "committed dependency changes update visibility and enablement");

  SettingsTransaction classic;
  classic.set("settings.theme.preset", std::string("classic"));
  const auto classic_result = state.apply(classic);
  expect(static_cast<bool>(classic_result),
         "predicate controller can change independently");
  SettingsTransaction disabled;
  disabled.set("settings.theme.particles", false);
  expect_error(state.apply(disabled), SettingsErrorCode::disabled,
               "visible but disabled settings reject user mutation");

  const auto stable_revision = state.snapshot().revision;
  SettingsTransaction no_change;
  no_change.set("settings.network.enabled", true);
  const auto no_change_result = state.apply(no_change);
  expect(no_change_result && no_change_result.value().changed_ids.empty() &&
             no_change_result.value().revision == stable_revision,
         "idempotent assignments do not advance the revision");

  SettingsTransaction action;
  action.invoke("settings.system.check-update");
  const auto invoked = state.apply(action);
  expect(invoked && invoked.value().changed_ids.empty() &&
             invoked.value().invoked_action_ids ==
                 std::vector<std::string>{"settings.system.check-update"} &&
             invoked.value().revision == stable_revision + 1,
         "actions are reported deterministically without persistent values");
}

void test_dirty_restart_and_resets() {
  const auto schema = make_schema();
  auto state = make_state(schema);

  SettingsTransaction changes;
  changes.set("settings.theme.preset", std::string("classic"))
      .set("settings.system.name", std::string("Cinema"))
      .set("settings.network.enabled", true);
  const auto changed = state.apply(changes);
  expect(changed && changed.value().dirty && changed.value().restart_pending,
         "dirty and restart-required flags derive from the saved baseline");
  expect(state.snapshot().find("settings.system.name")->dirty &&
             state.snapshot().find("settings.system.name")->restart_required,
         "per-setting snapshot flags identify restart-requiring changes");

  const auto appearance_reset = state.reset_scope("appearance");
  expect(appearance_reset &&
             std::get<std::string>(
                 state.value("settings.theme.preset").value()) == "original" &&
             std::get<std::string>(
                 state.value("settings.system.name").value()) == "Cinema",
         "scope reset changes exactly the requested scope");

  expect_error(state.reset_scope("unknown.scope"),
               SettingsErrorCode::invalid_scope,
               "unknown reset scopes are explicit errors");

  const auto all_reset = state.reset_all();
  expect(all_reset && !all_reset.value().dirty &&
             !all_reset.value().restart_pending,
         "global reset restores all mutable defaults");

  SettingsTransaction save_change;
  save_change.set("settings.system.name", std::string("Studio"));
  const auto save_applied = state.apply(save_change);
  expect(save_applied && save_applied.value().dirty,
         "new mutation becomes dirty before persistence acknowledgment");
  const auto before_save = save_applied.value().revision;
  const auto saved = state.mark_saved();
  expect(!saved.dirty && !saved.restart_pending &&
             saved.revision == before_save + 1,
         "mark_saved advances the baseline and clears restart pending");
}

void test_snapshot_restore_and_forward_compatibility() {
  const auto schema = make_schema();
  auto source = make_state(schema);
  SettingsTransaction changes;
  changes.set("settings.theme.preset", std::string("classic"))
      .set("settings.input.cursor-speed", std::int64_t{175})
      .set("settings.audio.music-volume", 0.3)
      .set("settings.system.name", std::string("Lounge"));
  const auto change_result = source.apply(changes);
  expect(static_cast<bool>(change_result),
         "source snapshot mutations are valid");
  const auto captured = source.snapshot();

  SettingsTransaction later;
  later.set("settings.input.cursor-speed", std::int64_t{25});
  const auto later_result = source.apply(later);
  expect(later_result &&
             std::get<std::int64_t>(
                 captured.find("settings.input.cursor-speed")->value) == 175,
         "deep snapshots remain unchanged after later state mutations");

  auto records = captured.records;
  records.push_back(
      {99, "settings.future.feature", SettingKind::boolean, true});
  auto restored = make_state(schema);
  const auto report = restored.restore(records);
  expect(report && report.value().restored_count == captured.records.size() &&
             report.value().diagnostics.size() == 1 &&
             report.value().diagnostics[0].setting_id ==
                 "settings.future.feature",
         "unknown future records are diagnosed and skipped compatibly");
  const auto restored_snapshot = restored.snapshot();
  expect(!restored_snapshot.dirty && !restored_snapshot.restart_pending &&
             restored_snapshot.records == captured.records,
         "restore reproduces the serial snapshot as a clean baseline");

  const auto before_invalid = restored.snapshot();
  auto invalid = captured.records;
  for (auto &record : invalid) {
    if (record.setting_id == "settings.input.cursor-speed")
      record.value = std::int64_t{126};
  }
  expect_error(restored.restore(invalid), SettingsErrorCode::invalid_value,
               "invalid known restore values are rejected");
  expect(restored.snapshot().records == before_invalid.records &&
             restored.snapshot().revision == before_invalid.revision,
         "failed restore is atomic and preserves the prior baseline");

  auto duplicate = captured.records;
  duplicate.push_back(captured.records.front());
  expect_error(restored.restore(duplicate), SettingsErrorCode::duplicate_record,
               "duplicate known records are rejected");

  auto wrong_kind = captured.records;
  wrong_kind.front().kind = SettingKind::text;
  expect_error(restored.restore(wrong_kind), SettingsErrorCode::kind_mismatch,
               "record kinds must match the immutable schema");

  auto wrong_version = captured.records;
  wrong_version.front().format_version = 2;
  expect_error(restored.restore(wrong_version),
               SettingsErrorCode::unsupported_record_version,
               "known records reject unsupported format versions");

  const std::vector<ValueRecord> overlay{
      {value_record_format_version, "settings.theme.preset",
       SettingKind::enumeration, std::string("classic")}};
  auto defaults = make_state(schema);
  const auto overlay_report = defaults.restore(overlay);
  expect(overlay_report &&
             std::get<std::int64_t>(
                 defaults.value("settings.input.cursor-speed").value()) == 100,
         "missing restore records deterministically fall back to defaults");
}

void test_null_schema_and_value_lookup() {
  expect_error(SettingsState::create(nullptr), SettingsErrorCode::null_schema,
               "state creation rejects null schemas");
  auto state = make_state(make_schema());
  expect_error(state.value("settings.absent"),
               SettingsErrorCode::unknown_setting,
               "value lookup reports unknown semantic IDs");
}

} // namespace

int main() {
  test_schema_and_deterministic_snapshot();
  test_invalid_schemas();
  test_atomic_mutation_and_value_validation();
  test_prospective_dependencies_and_actions();
  test_dirty_restart_and_resets();
  test_snapshot_restore_and_forward_compatibility();
  test_null_schema_and_value_lookup();
  if (failures != 0) {
    std::cerr << failures << " settings domain test(s) failed\n";
    return 1;
  }
  std::cout << "typed settings domain tests passed\n";
  return 0;
}
