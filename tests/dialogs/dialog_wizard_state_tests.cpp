#include "openxmb/dialogs/recovery.hpp"
#include "openxmb/dialogs/state_machine.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

using namespace openxmb::dialogs;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void expect_near(const double actual, const double expected,
                 const std::string_view message) {
  expect(std::abs(actual - expected) < 1e-12, message);
}

std::filesystem::path repository_root() {
  auto candidate = std::filesystem::path(__FILE__).parent_path();
  while (!candidate.empty()) {
    if (std::filesystem::exists(candidate /
                                "tests/fixtures/dialogs/pinned-inventory.txt"))
      return candidate;
    const auto parent = candidate.parent_path();
    if (parent == candidate)
      break;
    candidate = parent;
  }
  return std::filesystem::current_path();
}

std::string read_text(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

RouteDefinition route(RouteTrigger trigger, std::string action,
                      RouteOperation operation, std::string target = {}) {
  RouteDefinition value;
  value.trigger = trigger;
  value.action_id = std::move(action);
  value.operation = operation;
  value.target_id = std::move(target);
  return value;
}

DefinitionCatalog make_catalog() {
  DefinitionCatalog catalog;

  ScreenDefinition method;
  method.id = "wizard.connection.method";
  method.surface = SurfaceKind::wizard;
  method.kind = ScreenKind::choice;
  method.title_key = "wizard.connection.method.title";
  method.choices = {{"easy", "wizard.choice.easy", false},
                    {"details", "wizard.choice.details", false}};
  auto easy = route(RouteTrigger::confirm, "easy", RouteOperation::push,
                    "wizard.connection.name");
  easy.guards.push_back({"network.available", "true", false});
  method.routes = {std::move(easy),
                   route(RouteTrigger::confirm, "details", RouteOperation::push,
                         "dialog.information"),
                   route(RouteTrigger::cancel, {}, RouteOperation::cancel)};
  method.routes.back().result_code = "user-cancelled";
  expect(catalog.add(std::move(method)), "method definition inserted");

  ScreenDefinition name;
  name.id = "wizard.connection.name";
  name.surface = SurfaceKind::wizard;
  name.kind = ScreenKind::text_entry;
  name.title_key = "wizard.connection.name.title";
  name.fields = {{"network-name",
                  "wizard.field.network-name",
                  {},
                  true,
                  1,
                  32,
                  CharacterPolicy::printable_ascii}};
  name.buttons = {
      {"scan", "wizard.button.scan", ButtonRole::accept, true, true},
      {"back", "wizard.button.back", ButtonRole::cancel, false, true}};
  name.routes = {route(RouteTrigger::confirm, "scan", RouteOperation::push,
                       "wizard.connection.scanning"),
                 route(RouteTrigger::confirm, "back", RouteOperation::pop),
                 route(RouteTrigger::cancel, {}, RouteOperation::pop)};
  expect(catalog.add(std::move(name)), "text-entry definition inserted");

  ScreenDefinition scanning;
  scanning.id = "wizard.connection.scanning";
  scanning.surface = SurfaceKind::wizard;
  scanning.kind = ScreenKind::progress;
  scanning.title_key = "wizard.connection.scanning.title";
  scanning.resumable = false;
  scanning.cancel_allowed = false;
  auto scan_done = route(RouteTrigger::progress_complete, {},
                         RouteOperation::push, "dialog.scan-complete");
  scan_done.minimum_progress = 1.0;
  scanning.routes = {std::move(scan_done)};
  expect(catalog.add(std::move(scanning)), "progress definition inserted");

  ScreenDefinition result;
  result.id = "wizard.connection.result";
  result.surface = SurfaceKind::wizard;
  result.kind = ScreenKind::information;
  result.title_key = "wizard.connection.result.title";
  result.buttons = {
      {"done", "wizard.button.done", ButtonRole::accept, true, true}};
  result.routes = {
      route(RouteTrigger::confirm, "done", RouteOperation::complete)};
  result.routes.front().result_code = "connection-configured";
  expect(catalog.add(std::move(result)), "result definition inserted");

  ScreenDefinition information;
  information.id = "dialog.information";
  information.surface = SurfaceKind::dialog;
  information.kind = ScreenKind::information;
  information.title_key = "dialog.information.title";
  information.buttons = {
      {"ok", "dialog.button.ok", ButtonRole::accept, true, true}};
  information.routes = {
      route(RouteTrigger::confirm, "ok", RouteOperation::pop)};
  information.timeline = {.reveal_delay_ms = 50,
                          .reveal_duration_ms = 200,
                          .spinner_period_ms = 1000,
                          .initial_scale = 0.98,
                          .backdrop_opacity = 0.6};
  expect(catalog.add(std::move(information)), "information dialog inserted");

  ScreenDefinition scan_complete;
  scan_complete.id = "dialog.scan-complete";
  scan_complete.surface = SurfaceKind::dialog;
  scan_complete.kind = ScreenKind::message;
  scan_complete.title_key = "dialog.scan-complete.title";
  scan_complete.buttons = {
      {"continue", "dialog.button.continue", ButtonRole::accept, true, true}};
  scan_complete.routes = {route(RouteTrigger::confirm, "continue",
                                RouteOperation::replace,
                                "wizard.connection.result")};
  expect(catalog.add(std::move(scan_complete)), "nested message inserted");

  ScreenDefinition destructive;
  destructive.id = "dialog.erase-confirmation";
  destructive.surface = SurfaceKind::dialog;
  destructive.kind = ScreenKind::message;
  destructive.title_key = "dialog.erase-confirmation.title";
  destructive.buttons = {
      {"no", "dialog.button.no", ButtonRole::cancel, true, true},
      {"erase", "dialog.button.erase", ButtonRole::destructive, false, true}};
  destructive.routes = {
      route(RouteTrigger::confirm, "no", RouteOperation::cancel),
      route(RouteTrigger::confirm, "erase", RouteOperation::complete),
      route(RouteTrigger::cancel, {}, RouteOperation::cancel)};
  destructive.routes[0].result_code = "declined";
  destructive.routes[1].result_code = "erase-approved";
  destructive.routes[2].result_code = "dismissed";
  expect(catalog.add(std::move(destructive)), "destructive dialog inserted");

  ScreenDefinition chooser;
  chooser.id = "dialog.palette";
  chooser.kind = ScreenKind::choice;
  chooser.choices = {{"blue", "dialog.palette.blue", false}};
  chooser.routes = {
      route(RouteTrigger::confirm, "blue", RouteOperation::complete)};
  expect(catalog.add(std::move(chooser)), "choice dialog inserted");

  ScreenDefinition busy;
  busy.id = "dialog.creating";
  busy.kind = ScreenKind::busy;
  busy.cancel_allowed = false;
  auto busy_done =
      route(RouteTrigger::progress_complete, {}, RouteOperation::complete);
  busy_done.minimum_progress = 1.0;
  busy.routes = {std::move(busy_done)};
  expect(catalog.add(std::move(busy)), "busy dialog inserted");

  ScreenDefinition error;
  error.id = "dialog.operation-error";
  error.kind = ScreenKind::error;
  error.buttons = {{"ok", "dialog.button.ok", ButtonRole::accept, true, true}};
  error.routes = {route(RouteTrigger::confirm, "ok", RouteOperation::fail)};
  error.routes.front().result_code = "operation-failed";
  error.routes.front().message_key = "dialog.operation-error.body";
  expect(catalog.add(std::move(error)), "error dialog inserted");

  ScreenDefinition text;
  text.id = "dialog.rename";
  text.kind = ScreenKind::text_entry;
  text.fields = {
      {"name", "dialog.rename.name", {}, true, 1, 64, CharacterPolicy::any}};
  text.buttons = {{"ok", "dialog.button.ok", ButtonRole::accept, true, true}};
  text.routes = {route(RouteTrigger::confirm, "ok", RouteOperation::complete)};
  expect(catalog.add(std::move(text)), "text-entry dialog inserted");

  ScreenDefinition progress;
  progress.id = "dialog.progress";
  progress.kind = ScreenKind::progress;
  auto progress_done =
      route(RouteTrigger::progress_complete, {}, RouteOperation::complete);
  progress_done.minimum_progress = 1.0;
  progress.routes = {std::move(progress_done)};
  expect(catalog.add(std::move(progress)), "progress dialog inserted");

  return catalog;
}

SessionState apply(const DefinitionCatalog &catalog, SessionState state,
                   const Event &event, const std::uint64_t now_ms,
                   const std::string_view message) {
  auto result = transition(catalog, state, event, now_ms);
  expect(static_cast<bool>(result), message);
  expect(result.state.has_value(), "successful transition returns state");
  return result.state ? std::move(*result.state) : std::move(state);
}

void test_inventory_and_catalog_contract() {
  const auto inventory = read_text(
      repository_root() / "tests/fixtures/dialogs/pinned-inventory.txt");
  expect(inventory.find(
             "source_commit=5d4675366ad50deca14fe3d70a2aa646c341aee0") !=
             std::string::npos,
         "inventory is pinned to the audited source commit");
  expect(inventory.find("dialog_templates=51") != std::string::npos,
         "inventory records all 51 dialog templates");
  expect(inventory.find("wizard_screens=120") != std::string::npos,
         "inventory records all 120 wizard screens");

  const auto catalog = make_catalog();
  expect(catalog.validate().empty(), "valid generic definition graph accepted");
  expect(catalog.dialog_count() == 8,
         "one engine catalog owns all sample dialog kinds");
  expect(catalog.wizard_screen_count() == 4,
         "one engine catalog owns all sample wizard screens");

  DefinitionCatalog invalid;
  ScreenDefinition broken;
  broken.id = "dialog.broken";
  broken.kind = ScreenKind::choice;
  broken.routes = {route(RouteTrigger::confirm, "missing", RouteOperation::push,
                         "dialog.absent")};
  expect(invalid.add(std::move(broken)), "invalid fixture inserted");
  const auto errors = invalid.validate();
  expect(errors.size() >= 3,
         "shape, action, and unresolved targets are rejected together");
}

void test_focus_guards_nested_overlays_and_completion() {
  const auto catalog = make_catalog();
  auto started = start(catalog, "wizard.connection.method", 100);
  expect(started && started.state, "wizard starts from a catalog id");
  if (!started.state)
    return;
  auto state = std::move(*started.state);
  expect(state.focus_owner() == "wizard.connection.method" &&
             state.top()->focus.id == "easy",
         "root wizard owns focus at its configured default");

  const auto blocked = transition(catalog, state, {EventKind::confirm}, 120);
  expect(!blocked && blocked.error == MachineError::guard_rejected &&
             !blocked.state,
         "guarded transition fails atomically without replacement state");

  state = apply(catalog, std::move(state),
                {EventKind::set_variable, 0, "network.available", "true"}, 125,
                "guard variable set");
  state = apply(catalog, std::move(state), {EventKind::navigate, 1}, 130,
                "selection advances");
  expect(state.top()->focus.id == "details" &&
             state.top()->selected_choice == 1,
         "choice focus and selected index remain synchronized");
  state = apply(catalog, std::move(state), {EventKind::confirm}, 140,
                "details overlay opens");
  expect(state.stack.size() == 2 && state.focus_owner() == "dialog.information",
         "nested overlay exclusively owns top-frame focus");
  const auto parent_selection = state.stack.front().selected_choice;
  state = apply(catalog, std::move(state), {EventKind::confirm}, 150,
                "information overlay closes");
  expect(state.stack.size() == 1 &&
             state.focus_owner() == "wizard.connection.method" &&
             state.top()->selected_choice == parent_selection,
         "closing overlay restores the untouched parent frame");

  state = apply(catalog, std::move(state), {EventKind::set_focus, 0, "easy"},
                160, "focus returns to easy path");
  state = apply(catalog, std::move(state), {EventKind::confirm}, 170,
                "guarded name screen opens");
  expect(state.focus_owner() == "wizard.connection.name" &&
             state.top()->focus.kind == FocusKind::field,
         "text-entry field receives focus ownership");

  const auto invalid = transition(catalog, state, {EventKind::confirm}, 180);
  expect(!invalid && invalid.error == MachineError::validation_failed &&
             invalid.validation.size() == 1 &&
             invalid.validation.front().code == ValidationErrorCode::required,
         "required field validation blocks advancement with a strict issue");
  state = apply(catalog, std::move(state),
                {EventKind::set_field, 0, "network-name", "Home Network"}, 190,
                "valid network name stored");
  state = apply(catalog, std::move(state), {EventKind::confirm}, 200,
                "default button advances from the focused field");
  expect(state.focus_owner() == "wizard.connection.scanning" &&
             state.stack.size() == 3,
         "non-resumable progress frame is pushed by data route");
  const auto early =
      transition(catalog, state, {EventKind::complete_progress}, 210);
  expect(!early && early.error == MachineError::progress_incomplete,
         "progress completion is guarded by its threshold");
  const auto swallowed = transition(catalog, state, {EventKind::cancel}, 215);
  expect(!swallowed && swallowed.error == MachineError::invalid_event,
         "non-cancellable progress screen swallows cancellation");
  state = apply(catalog, std::move(state),
                {EventKind::set_progress, 0, {}, {}, 1.0}, 220,
                "progress reaches completion");
  state = apply(catalog, std::move(state), {EventKind::complete_progress}, 230,
                "completion overlay opens");
  expect(state.focus_owner() == "dialog.scan-complete" &&
             state.stack.size() == 4,
         "completion overlay nests safely above progress");

  auto cancel_path = apply(catalog, state, {EventKind::cancel}, 240,
                           "completion overlay cancels");
  expect(cancel_path.focus_owner() == "wizard.connection.name" &&
             cancel_path.stack.size() == 2,
         "back skips the non-resumable progress frame");

  state = apply(catalog, std::move(state), {EventKind::confirm}, 250,
                "completion overlay advances");
  expect(state.focus_owner() == "wizard.connection.result",
         "overlay replacement enters result screen");
  state = apply(catalog, std::move(state), {EventKind::confirm}, 260,
                "wizard completes");
  expect(!state.active() && state.stack.empty() &&
             state.outcome.kind == OutcomeKind::completed &&
             state.outcome.result_code == "connection-configured" &&
             state.outcome.action_id == "done",
         "completion returns an explicit terminal result");
}

void test_button_and_terminal_semantics() {
  const auto catalog = make_catalog();
  auto declined = start(catalog, "dialog.erase-confirmation", 1);
  expect(declined && declined.state && declined.state->top()->focus.id == "no",
         "default cancel-role button owns initial focus");
  if (declined.state) {
    auto result = transition(catalog, *declined.state, {EventKind::confirm}, 2);
    expect(result && result.state &&
               result.state->outcome.kind == OutcomeKind::cancelled &&
               result.state->outcome.result_code == "declined",
           "confirming cancel-role default produces explicit cancellation");
  }

  auto approved = start(catalog, "dialog.erase-confirmation", 10);
  if (approved.state) {
    auto state =
        apply(catalog, std::move(*approved.state), {EventKind::navigate, 1}, 11,
              "destructive button focused");
    expect(state.top()->focus.id == "erase",
           "navigation can focus the destructive action");
    state = apply(catalog, std::move(state), {EventKind::confirm}, 12,
                  "destructive action confirmed");
    expect(state.outcome.kind == OutcomeKind::completed &&
               state.outcome.result_code == "erase-approved" &&
               state.outcome.action_id == "erase",
           "destructive action is distinguishable in completion result");
  }

  auto external = start(catalog, "dialog.creating", 20);
  if (external.state) {
    auto failed = transition(catalog, *external.state,
                             {EventKind::fail,
                              0,
                              {},
                              {},
                              0.0,
                              "storage-unavailable",
                              "dialog.error.storage-unavailable"},
                             21);
    expect(failed && failed.state &&
               failed.state->outcome.kind == OutcomeKind::failed &&
               failed.state->outcome.result_code == "storage-unavailable",
           "external failures produce explicit error results");
  }
}

void test_animation_sampling() {
  const auto catalog = make_catalog();
  auto started = start(catalog, "dialog.information", 100);
  if (!started.state)
    return;
  const auto *definition = catalog.find("dialog.information");
  expect(definition != nullptr, "animation definition found");
  if (!definition)
    return;
  const auto before = sample_animation(*definition, *started.state->top(), 149);
  const auto boundary =
      sample_animation(*definition, *started.state->top(), 150);
  const auto middle = sample_animation(*definition, *started.state->top(), 250);
  const auto settled =
      sample_animation(*definition, *started.state->top(), 10000);
  expect_near(before.reveal_linear, 0.0, "reveal waits for its delay");
  expect_near(boundary.reveal_linear, 0.0, "delay boundary is deterministic");
  expect_near(middle.reveal_linear, 0.5, "timeline samples linear midpoint");
  expect_near(middle.reveal_eased, 0.875,
              "timeline uses deterministic ease-out cubic");
  expect_near(middle.scale, 0.9975, "scale follows the same eased sample");
  expect_near(middle.backdrop_opacity, 0.525,
              "backdrop reveal follows the same eased sample");
  expect_near(middle.spinner_phase, 0.15,
              "spinner phase derives only from monotonic time");
  expect_near(settled.opacity, 1.0, "reveal settles exactly at full opacity");

  auto malformed = *definition;
  malformed.timeline.reveal_duration_ms = 0;
  malformed.timeline.spinner_period_ms = 0;
  const auto hardened = sample_animation(malformed, *started.state->top(), 250);
  expect(std::isfinite(hardened.reveal_linear) &&
             std::isfinite(hardened.spinner_phase),
         "sampler remains defined for an unvalidated zero-duration timeline");
}

void test_validation_policies_and_invalid_focus() {
  ScreenDefinition definition;
  definition.id = "dialog.validation-test";
  definition.kind = ScreenKind::text_entry;
  definition.fields = {
      {"pin", "field.pin", {}, true, 4, 4, CharacterPolicy::numeric},
      {"address", "field.address", {}, true, 7, 15, CharacterPolicy::ipv4},
      {"token", "field.token", {}, true, 2, 8, CharacterPolicy::hexadecimal},
      {"label",
       "field.label",
       {},
       true,
       1,
       8,
       CharacterPolicy::printable_ascii},
      {"optional",
       "field.optional",
       {},
       false,
       4,
       8,
       CharacterPolicy::numeric}};
  FrameState frame;
  frame.definition_id = definition.id;
  frame.fields = {{"pin", "12x"},
                  {"address", "999.1.2.3"},
                  {"token", "xz"},
                  {"label", std::string("bad\x01", 4)},
                  {"optional", ""}};
  const auto invalid = validate_fields(definition, frame);
  expect(invalid.size() == 4,
         "length, IPv4, hexadecimal, and printable policies all reject");
  frame.fields = {{"pin", "1234"},
                  {"address", "192.168.1.1"},
                  {"token", "aF09"},
                  {"label", "valid"},
                  {"optional", ""}};
  expect(validate_fields(definition, frame).empty(),
         "all field policies accept valid boundary values");

  const auto catalog = make_catalog();
  const auto started = start(catalog, "dialog.erase-confirmation", 1);
  if (started.state) {
    const auto bad_focus =
        transition(catalog, *started.state,
                   {EventKind::set_focus, 0, "not-a-focus-target"}, 2);
    expect(!bad_focus && bad_focus.error == MachineError::invalid_focus &&
               !bad_focus.state,
           "invalid focus transition fails atomically");
  }
}

void test_recovery_round_trip_and_rejection() {
  const auto catalog = make_catalog();
  auto started = start(catalog, "wizard.connection.method", 1000);
  if (!started.state)
    return;
  auto state = apply(catalog, std::move(*started.state),
                     {EventKind::set_variable, 0, "network.available", "true"},
                     1010, "recovery variable stored");
  state = apply(catalog, std::move(state), {EventKind::confirm}, 1020,
                "recovery text screen opened");
  state =
      apply(catalog, std::move(state),
            {EventKind::set_field, 0, "network-name", std::string("A\0B", 3)},
            1030, "binary-safe field stored");

  const auto encoded = serialize_recovery(state);
  expect(encoded == serialize_recovery(state),
         "recovery serialization is byte-for-byte deterministic");
  const auto restored = restore_recovery(catalog, encoded);
  expect(restored && restored.state && *restored.state == state,
         "nested active state recovers exactly, including embedded nulls");

  auto trailing = encoded;
  trailing.push_back('x');
  const auto trailing_result = restore_recovery(catalog, trailing);
  expect(!trailing_result && trailing_result.error &&
             trailing_result.error->code == RecoveryErrorCode::malformed,
         "trailing recovery bytes are rejected");
  const auto version = restore_recovery(catalog, "unknown");
  expect(!version && version.error &&
             version.error->code == RecoveryErrorCode::unsupported_version,
         "unknown recovery versions are rejected");

  auto invalid_state = state;
  invalid_state.stack.back().focus.id = "not-a-focus-target";
  const auto invalid_result =
      restore_recovery(catalog, serialize_recovery(invalid_state));
  expect(!invalid_result && invalid_result.error &&
             invalid_result.error->code == RecoveryErrorCode::invalid_state,
         "recovery validates focus ownership against current definitions");

  DefinitionCatalog reduced;
  ScreenDefinition only;
  only.id = "wizard.connection.method";
  only.surface = SurfaceKind::wizard;
  only.kind = ScreenKind::information;
  expect(reduced.add(std::move(only)), "reduced catalog fixture inserted");
  const auto missing = restore_recovery(reduced, encoded);
  expect(!missing && missing.error &&
             missing.error->code == RecoveryErrorCode::missing_definition,
         "recovery refuses frames missing from the current catalog");
}

} // namespace

int main() {
  test_inventory_and_catalog_contract();
  test_focus_guards_nested_overlays_and_completion();
  test_button_and_terminal_semantics();
  test_animation_sampling();
  test_validation_policies_and_invalid_focus();
  test_recovery_round_trip_and_rejection();
  if (failures) {
    std::cerr << failures << " dialog/wizard state test(s) failed\n";
    return 1;
  }
  std::cout << "dialog/wizard state tests passed\n";
  return 0;
}
