#include "openxmb/xmb/catalog.hpp"
#include "openxmb/xmb/navigation_state.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::filesystem::path repository_root() {
  auto candidate = std::filesystem::path(__FILE__).parent_path();
  while (!candidate.empty()) {
    if (std::filesystem::exists(candidate / "assets/xmb/catalog/en.json"))
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

bool replace_once(std::string &text, std::string_view from,
                  std::string_view to) {
  const auto position = text.find(from);
  if (position == std::string::npos)
    return false;
  text.replace(position, from.size(), to);
  return true;
}

void test_catalog() {
  using namespace openxmb::xmb;
  const auto path = repository_root() / "assets/xmb/catalog/en.json";
  const auto result = load_catalog_file(path);
  expect(static_cast<bool>(result), "English native catalogue loads");
  if (!result) {
    if (result.error)
      std::cerr << result.error->path << ": " << result.error->message << '\n';
    return;
  }
  const auto &catalog = *result.value;
  expect(catalog.source_commit == "5d4675366ad50deca14fe3d70a2aa646c341aee0",
         "catalogue is pinned to the audited source commit");
  expect(catalog.nodes.size() == 214,
         "nine roots plus 205 reachable source items are present");
  expect(catalog.dialogs.size() == 51,
         "static and runtime dialog templates are complete");
  expect(catalog.wizards.size() == 120, "all wizard screens are present");
  expect(catalog.controls.size() == 79,
         "viewer/player and contextual option controls are present");
  expect(catalog.landings.size() == 4,
         "static and generic runtime landing templates are present");
  expect(catalog.strings.size() == 1391,
         "every human-facing field has an English lookup");

  const std::vector<std::string> expected_roots{
      "category.users",   "category.settings", "category.photo",
      "category.music",   "category.video",    "category.game",
      "category.network", "category.online",   "category.friends"};
  expect(catalog.root_order == expected_roots,
         "root order matches the pinned source");
  const std::size_t expected_children[]{3, 16, 4, 3, 5, 9, 4, 5, 8};
  for (std::size_t i = 0; i < expected_roots.size(); ++i) {
    const auto *node = catalog.find_node(expected_roots[i]);
    expect(node && node->children.size() == expected_children[i],
           "root direct-child count matches source");
  }
  const auto *settings = catalog.find_node("category.settings");
  expect(settings && settings->default_selection == 5,
         "Settings defaults to System Settings");

  const auto *format =
      catalog.find_node("category.settings.system.settings.format.utility");
  expect(format && format->action_id == "wizard.fmt_method" &&
             format->safety == SafetyClass::dangerous,
         "deep destructive setting routes to its classified wizard");
  const auto *connection = catalog.find_node(
      "category.settings.network.settings.internet.connection.settings");
  expect(connection && connection->action_id == "wizard.wiz_intro" &&
             connection->capability == "network",
         "deep network setting has stable routing and capability metadata");
  const auto *message_box = catalog.find_node("category.friends.message.box");
  expect(message_box && message_box->children.size() == 3,
         "nested Friends message structure is represented");

  const auto *wlan = catalog.find_wizard("wizard.wlan_security");
  expect(wlan && wlan->choices.size() == 8 && wlan->next_ids.size() == 3,
         "wireless security chooser covers every branch");
  const auto *zones = catalog.find_wizard("wizard.tz_globe");
  expect(zones && zones->choices.size() == 57,
         "time-zone globe contains every audited city entry");
  const auto *video_resolution = catalog.find_wizard("wizard.vid_resolution");
  expect(video_resolution && video_resolution->choices.size() == 6 &&
             video_resolution->next_ids ==
                 std::vector<std::string>{"wizard.vid_test"},
         "video-output checklist and finish row are represented");

  const auto colour = catalog.dialogs.find("dialog.colour");
  expect(colour != catalog.dialogs.end() && colour->second.side_panel &&
             colour->second.choices.size() == 21,
         "colour side panel contains the complete source palette");
  const auto photo_effect =
      catalog.controls.find("control.photo.viewer.effect");
  expect(photo_effect != catalog.controls.end() &&
             photo_effect->second.choices.size() == 3,
         "photo effect submenu is encoded");
  const auto video_screen =
      catalog.controls.find("control.video.player.screenmode");
  expect(video_screen != catalog.controls.end() &&
             video_screen->second.choices.size() == 5,
         "video screen-mode submenu is encoded");

  expect(!load_catalog_json("{").value.has_value(),
         "malformed JSON is reported, not thrown");
  expect(
      !load_catalog_file(path.parent_path() / "missing.json").value.has_value(),
      "missing catalogue is reported, not thrown");

  auto duplicate = read_text(path);
  expect(replace_once(duplicate, "\"id\": \"category.game\"",
                      "\"id\": \"category.friends\""),
         "duplicate-id rejection fixture is created");
  const auto duplicate_result =
      load_catalog_json(duplicate, "duplicate-fixture");
  expect(!duplicate_result && duplicate_result.error &&
             duplicate_result.error->code == CatalogErrorCode::duplicate_id,
         "duplicate semantic IDs are rejected deterministically");

  auto missing_translation = read_text(path);
  expect(replace_once(missing_translation,
                      "\"label_key\": \"category.friends.add.a.friend.label\"",
                      "\"label_key\": \"translation.intentionally.missing\""),
         "translation rejection fixture is created");
  const auto translation_result =
      load_catalog_json(missing_translation, "translation-fixture");
  expect(!translation_result && translation_result.error &&
             translation_result.error->code ==
                 CatalogErrorCode::missing_translation,
         "untranslated human-facing fields are rejected deterministically");

  auto unsupported = read_text(path);
  expect(replace_once(unsupported, "\"schema_version\": 1",
                      "\"schema_version\": 99"),
         "schema rejection fixture is created");
  const auto unsupported_result =
      load_catalog_json(unsupported, "schema-fixture");
  expect(!unsupported_result && unsupported_result.error &&
             unsupported_result.error->code ==
                 CatalogErrorCode::unsupported_schema,
         "unknown catalogue schemas are rejected deterministically");
}

void test_navigation() {
  using namespace openxmb::xmb;
  NavigationState state;
  expect(state.valid() && state.top().layer == NavigationLayer::boot,
         "navigation starts at boot");

  auto apply = [&](NavigationEvent event) {
    auto result = transition(state, event);
    expect(static_cast<bool>(result), "navigation transition succeeds");
    if (result && result.state)
      state = std::move(*result.state);
    return result.changed;
  };

  apply({NavigationEventKind::complete_boot, "", 0, true});
  apply({NavigationEventKind::select_category, "category.settings", 1, true});
  apply({NavigationEventKind::enter_menu, "category.settings.network.settings",
         2, true});
  apply({NavigationEventKind::open_wizard, "wizard.wiz_intro", 0, true});
  apply({NavigationEventKind::advance_wizard, "wizard.wiz_method", 0, true});
  apply({NavigationEventKind::advance_wizard, "wizard.easy_wired_check", 0,
         false});
  apply({NavigationEventKind::advance_wizard, "wizard.easy_settings_list", 1,
         true});
  apply({NavigationEventKind::open_osk, "wizard.field.ssid", 0, true});
  expect(state.top().layer == NavigationLayer::osk,
         "OSK has highest active back priority");
  apply({NavigationEventKind::back, "", 0, true});
  expect(state.top().id == "wizard.easy_settings_list",
         "back closes OSK before wizard");
  apply({NavigationEventKind::back, "", 0, true});
  expect(state.top().id == "wizard.wiz_method",
         "back skips non-resumable wizard progress routes");

  apply(
      {NavigationEventKind::open_modal, "dialog.internet.connection", 0, true});
  apply({NavigationEventKind::open_option_panel, "control.content.options", 0,
         true});
  apply({NavigationEventKind::back, "", 0, true});
  expect(state.top().layer == NavigationLayer::modal,
         "back closes option panel before the underlying modal");
  apply({NavigationEventKind::back, "", 0, true});
  expect(state.top().layer == NavigationLayer::wizard,
         "second back closes the modal");

  apply({NavigationEventKind::reset_to_root, "", 0, true});
  apply({NavigationEventKind::open_music, "media.music.now-playing", 0, true});
  apply({NavigationEventKind::open_option_panel, "control.music.player", 0,
         true});
  apply({NavigationEventKind::back, "", 0, true});
  expect(state.top().layer == NavigationLayer::music,
         "player control panel closes before the player");
  apply({NavigationEventKind::back, "", 0, true});
  expect(state.top().layer == NavigationLayer::root,
         "media player returns to root");

  const auto invalid = transition(state, {NavigationEventKind::advance_wizard,
                                          "wizard.wiz_method", 0, true});
  expect(!invalid && invalid.error == NavigationError::invalid_event &&
             !invalid.state.has_value(),
         "invalid transition is contained and leaves state unchanged");
  const auto root_back =
      transition(state, {NavigationEventKind::back, "", 0, true});
  expect(root_back && root_back.state && !root_back.changed &&
             *root_back.state == state,
         "back at root is a stable no-op");
}

void test_files_menu_closed_contract() {
  const auto source = read_text(repository_root() / "src/menu/files_menu.cpp");
  const auto main_menu =
      read_text(repository_root() / "src/app/components/main_menu.cpp");
  expect(source.contains("files_menu::get_submenus_count"),
         "files menu implementation fixture is readable");
  expect(!source.contains("return is_open ? entries.size() : 1;"),
         "closed file menus do not advertise a phantom submenu after clearing entries");
  expect(main_menu.contains("files_menu_profile::music"),
         "file-backed Music category declares a media-specific profile");
  expect(source.contains("compat/xmb-ui-compat/images/icon_fw_track.png"),
         "Music file rows prefer xmb-web's track icon");
  expect(source.contains("filter_for_profile(profile, info)"),
         "file-backed media categories filter to relevant local media");
}

} // namespace

int main() {
  test_catalog();
  test_navigation();
  test_files_menu_closed_contract();
  if (failures) {
    std::cerr << failures << " catalogue/state test(s) failed\n";
    return 1;
  }
  std::cout << "native catalogue/state tests passed\n";
  return 0;
}
