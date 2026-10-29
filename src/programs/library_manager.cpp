module;

#include "openxmb/desktop/library.hpp"
#include "openxmb/music/player.hpp"
#include "openxmb/desktop/runners.hpp"

#include <SDL2/SDL.h>
#undef main
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module openxmb.app;

import :library_manager;
import :game_presentation;
import :main;
import :text_entry;
import :programs;
import glm;
import auroreengine;
import vulkan_hpp;
import openxmb.utils;

namespace programs {
namespace desktop = openxmb::desktop;
namespace fs = std::filesystem;
namespace {
desktop::GameDiscoveryOptions configured_discovery_options() {
  desktop::GameDiscoveryOptions options;
  options.rpcs3_executable=fs::u8path(config::CONFIG.preference("runner.rpcs3.path"));
  return options;
}
std::string elide(std::string text, std::size_t length) {
  if (text.size() <= length) return text;
  while (length && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) --length;
  return text.substr(0, length) + "...";
}
template<typename T> T find(const std::vector<T> &values, const std::string &id) {
  for (const auto &value : values) if (value.id == id) return value;
  throw std::runtime_error("The selected local item no longer exists");
}
}

library_manager::library_manager(std::string section,
    auroreengine::resource_loader &loader, profile_callback callback)
    : preview_(std::make_unique<app::game_presentation>(loader)), loader_(loader), switch_profile_(std::move(callback)) {
  if (section == "users" || section == "profiles") page_ = Page::users;
  else if (section == "custom" || section == "sections") page_ = Page::custom;
  else if (section == "friends") page_ = Page::friends;
  else if (section == "account" || section == "network") page_ = Page::account;
  else if (section == "trophies") page_ = Page::trophies;
  else if (section == "runners") page_ = Page::runners;
  root_page_ = page_;
  attempt([&] { library_ = std::make_unique<desktop::Library>(); refresh(); });
}
library_manager::~library_manager() {
  preview_.reset();
  if (artwork_ready_.valid()) artwork_ready_.wait();
  if (artwork_download_.valid()) artwork_download_.wait();
}
void library_manager::attempt(std::function<void()> action) {
  try { action(); }
  catch (const std::exception &error) { message_ = error.what(); }
}
void library_manager::enter(Page page) {
  history_.push_back(page_); page_ = page; selected_ = 0; refresh();
}
void library_manager::leave() {
  if (history_.empty()) return;
  page_ = history_.back(); history_.pop_back(); selected_ = 0; refresh();
}
void library_manager::ask(std::string title, std::string initial,
    std::function<void(std::string)> accept) {
  pending_ = [this, title = std::move(title), initial = std::move(initial), accept = std::move(accept)](app::shell *shell) mutable {
    shell->emplace_overlay<app::text_entry_overlay>(std::move(title), std::move(initial),
      [this, accept = std::move(accept)](std::string value) mutable {
        attempt([&] { accept(std::move(value)); refresh(); });
      });
  };
}
void library_manager::confirm(std::string title, std::function<void()> accept) {
  confirm_title_ = std::move(title); confirm_action_ = std::move(accept); enter(Page::confirm);
}
void library_manager::open_file(fs::path file) {
  preview_suspended_ = true; preview_->stop();
  pending_ = [this, file = std::move(file)](app::shell *shell) {
    attempt([&] {
      const auto handlers = get_open_infos(file, file_info(file));
      for (const auto &handler : handlers) if (!handler.is_external) {
        shell->push_overlay(handler.create(file, loader_)); return;
      }
      throw std::runtime_error("No native viewer is registered for this file type");
    });
  };
}
result library_manager::tick(app::shell *shell) {
  if (closing_) {
    preview_->stop();
    if (menu_dirty_) shell->queue_menu_reload();
    return result::close | result::back_sound;
  }
  preview_->choose(!preview_suspended_ && selected_ < rows_.size() ? rows_[selected_].game : nullptr);
  preview_->tick(shell->get_window());
  if (pending_) { auto request = std::move(pending_); pending_ = {}; request(shell); }
  if (artwork_download_.valid() && artwork_download_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    attempt([&] { message_ = "Artwork saved: " + artwork_download_.get().filename().string(); refresh(); });
  const auto target = selected_ < rows_.size() ? rows_[selected_].artwork : fs::path{};
  if (target != loaded_artwork_) {
    if (artwork_ready_.valid()) artwork_ready_.wait();
    // Submitted frames may still sample the old texture; drain before destroying it.
    if (artwork_) shell->get_window()->waitIdle();
    artwork_.reset(); loaded_artwork_ = target;
    std::error_code error;
    if (!target.empty() && fs::is_regular_file(target, error)) {
      artwork_ = std::make_shared<auroreengine::texture>(loader_.getDevice(), loader_.getAllocator());
      artwork_ready_ = loader_.loadTexture(artwork_.get(), target);
    }
  }
  return result::success;
}
void library_manager::prerender(vk::CommandBuffer command, int frame, app::shell *shell) { preview_->upload(command, frame, shell->get_window()); }
result library_manager::on_event(const app::event &event) {
  if (event.action != action::none) preview_suspended_ = false;
  if (event.action == action::cancel) {
    if (page_ == Page::folder && !folder_history_.empty()) {
      folder_ = folder_history_.back(); folder_history_.pop_back(); selected_ = 0; attempt([&] { refresh(); }); return result::success;
    }
    if (history_.empty()) { closing_ = true; return result::success; }
    attempt([&] { leave(); }); return result::success | result::back_sound;
  }
  if (event.action == action::extra) { attempt([&] { refresh(); }); return result::success; }
  if (rows_.empty()) return result::success;
  if (event.action == action::up) selected_ = (selected_ + rows_.size() - 1) % rows_.size();
  else if (event.action == action::down) selected_ = (selected_ + 1) % rows_.size();
  else if (event.action == action::ok) {
    const auto action = rows_[selected_].activate;
    if (action) attempt(action);
    return result::success | result::ok_sound;
  }
  return result::success;
}
std::string library_manager::heading() const {
  switch (page_) {
  case Page::users: return "Local users"; case Page::profile: return "User profile";
    case Page::games: return "Game library"; case Page::game: return "Game details"; case Page::roots: return "Game library folders";
    case Page::runners: return "Game runners";
  case Page::custom: return "Custom sections"; case Page::section: return "Custom section";
  case Page::friends: return "Friends - local simulation"; case Page::person: return "Local friend";
  case Page::account: return "Local account - no online sign-in"; case Page::trophies: return "Local trophies";
  case Page::trophy: return "Local trophy"; case Page::folder: return folder_.filename().string(); case Page::confirm: return confirm_title_;
  }
  return "Library";
}
void library_manager::render(auroreengine::gui_renderer &r, app::shell *shell) {
  preview_->draw_background(r, 0.80f);
  r.draw_rect({0.06f, 0.08f}, {0.88f, 0.84f}, {0.025f, 0.035f, 0.065f, 0.82f});
  preview_->draw_movie(r);
  r.draw_text(heading(), 0.10f, 0.14f, 0.048f);
  if (library_) r.draw_text("Profile: " + library_->active_profile().name, 0.10f, 0.21f, 0.025f);
  const std::size_t first = selected_ > 5 ? selected_ - 5 : 0;
  for (std::size_t i = first; i < std::min(rows_.size(), first + 7); ++i) {
    const float y = 0.29f + static_cast<float>(i - first) * 0.071f;
    if (i == selected_) r.draw_rect({0.09f, y - 0.012f}, {0.65f, 0.063f}, {0.25f, 0.45f, 0.65f, 0.45f});
    r.draw_text(elide(rows_[i].title, 66), 0.11f, y, 0.031f);
    if (!rows_[i].detail.empty()) r.draw_text(elide(rows_[i].detail, 90), 0.11f, y + 0.032f, 0.019f, {0.75f, 0.80f, 0.88f, 1.0f});
  }
  if (artwork_ && artwork_->loaded) {
    const float aspect = static_cast<float>(artwork_->width) / std::max(1, artwork_->height);
    const float height = std::min(preview_->has_movie() ? 0.13f : 0.30f, 0.16f * static_cast<float>(r.aspect_ratio) / std::max(aspect, 0.01f));
    r.draw_image(artwork_->imageView.get(), 0.76f, preview_->has_movie() ? 0.65f : 0.30f, height * aspect, height);
  }
  if (rows_.empty()) r.draw_text("No entries yet. Use the actions above to add one.", 0.11f, 0.31f, 0.03f);
  r.draw_text(elide(message_, 112), 0.10f, 0.82f, 0.023f, {0.75f, 0.85f, 1.0f, 1.0f});
  render_controller_buttons(shell, r, 0.5f, 0.89f, std::array{
    std::pair{action::ok, std::string_view{"Select"}}, std::pair{action::extra, std::string_view{"Refresh"}}, std::pair{action::cancel, std::string_view{"Back"}}});
}
void library_manager::edit_game(std::string id) { chosen_id_ = std::move(id); enter(Page::game); }
void library_manager::game_field(std::string title, std::string value,
    std::function<void(desktop::Game &, std::string)> set) {
  ask(std::move(title), std::move(value), [this, set = std::move(set)](std::string result) {
    auto game = find(library_->games(), chosen_id_); set(game, std::move(result)); library_->save_game(std::move(game));
  });
}
void library_manager::refresh() {
  rows_.clear();
  if (!library_) return;
  auto add = [&](std::string title, std::string detail, std::function<void()> action, fs::path artwork = {}, std::shared_ptr<const desktop::Game> game = {}) {
    rows_.push_back({std::move(title), std::move(detail), std::move(action), std::move(artwork), std::move(game)});
  };
  switch (page_) {
  case Page::users:
    add("Create local user", "Separate settings, themes, game library and local social data", [this] {
      ask("New user name", "New User", [this](std::string name) {
        fs::path baseline;
        if (const auto *configured = std::getenv("OPENXMB_CONFIG"); configured && *configured) baseline = configured;
        if (baseline.empty()) baseline = library_->config_path(library_->active_profile().id);
        const auto created = library_->create_profile(std::move(name), baseline);
        chosen_id_ = created.id; enter(Page::profile);
      });
    });
    for (const auto &profile : library_->profiles()) add(profile.name + (profile.id == library_->active_profile().id ? " (active)" : ""), "Local profile", [this, id = profile.id] { chosen_id_ = id; enter(Page::profile); }, profile.avatar);
    break;
  case Page::profile: {
    auto profile = find(library_->profiles(), chosen_id_);
    add("Switch to " + profile.name, library_->config_path(profile.id).string(), [this, id = profile.id] {
      const auto path = library_->switch_profile(id);
      if (switch_profile_) switch_profile_(path);
      message_ = "Active local user changed. Settings and themes use this profile."; refresh();
    }, profile.avatar);
    add("Rename user", profile.name, [this, profile] { ask("User name", profile.name, [this, profile](std::string value) mutable { profile.name = std::move(value); library_->update_profile(profile); }); });
    add("Avatar image", profile.avatar.string(), [this, profile] { ask("Avatar image path", profile.avatar.string(), [this, profile](std::string value) mutable { profile.avatar = std::move(value); library_->update_profile(profile); }); });
    add("Delete local user", "Archives the user's data; at least one profile is retained", [this, id = profile.id] {
      confirm("Archive this user profile?", [this, id] {
        const bool active = library_->active_profile().id == id;
        library_->remove_profile(id);
        if (active && switch_profile_) switch_profile_(library_->config_path(library_->active_profile().id));
      });
    });
    break;
  }
  case Page::runners: {
    for(const auto& runner:desktop::discover_runners(desktop::runner_options_from_preferences(config::CONFIG.preferences))) {
      const auto key="runner."+runner.id+".path";
      add(runner.name,runner?runner.executable.string():runner.message,[this,key,runner] {
        ask(runner.name+" executable (empty for automatic)",config::CONFIG.preference(key),[key](std::string value) {
          if(!value.empty()&&!fs::is_regular_file(fs::u8path(value)))throw std::runtime_error("Choose an existing executable file, or leave empty for automatic discovery.");
          const auto previous=config::CONFIG.preferences;
          if(value.empty())config::CONFIG.preferences.erase(key);else config::CONFIG.preferences[key]=std::move(value);
          if(!config::CONFIG.save_config_checked()) {config::CONFIG.preferences=previous;throw std::runtime_error("Could not save runner settings.");}
        });
      });
    }
    break;
  }
  case Page::games:
    add("Game runners", "Detected emulators and executable overrides", [this] {enter(Page::runners);});
    add("Add game manually", "Choose executable, arguments, artwork, preview video and music", [this] { desktop::Game value; value.title = "New Game"; edit_game(library_->save_game(std::move(value)).id); });
    add("Manage game folders", "Configure scan roots without moving your games", [this] { enter(Page::roots); });
    add("Scan game folders", "Imports game manifests, desktop entries and native executables", [this] { message_ = "Added " + std::to_string(library_->scan_games(configured_discovery_options())) + " game(s)."; refresh(); });
    for (const auto &game : library_->games()) add((game.favorite ? "* " : "") + game.title, game.genre + (game.year ? "  " + std::to_string(game.year) : "") + "  Played " + std::to_string(game.play_count) + " times", [this, id = game.id] { edit_game(id); }, game.icon.empty() ? game.background : game.icon, std::make_shared<const desktop::Game>(game));
    break;
  case Page::game: {
    auto game = find(library_->games(), chosen_id_);
    add("Launch " + game.title, game.executable.string(), [this, game] {
      preview_suspended_=true; preview_->stop();
      pending_=[game](app::shell* shell) {static_cast<void>(shell->launch_game(game));};
    }, game.icon);
    add("Title", game.title, [this, game] { game_field("Game title", game.title, [](auto &g, auto v) { g.title = std::move(v); }); });
    add("Runner",game.runner.empty()?"native":game.runner,[this,game] {game_field("Runner: native, rpcs3, pcsx2, steam, retroarch, dolphin, duckstation, ppsspp",game.runner,[](auto& g,auto value) {
      const std::array<std::string_view,8> names{"native","rpcs3","pcsx2","steam","retroarch","dolphin","duckstation","ppsspp"};
      if(!value.empty()&&std::ranges::find(names,value)==names.end())throw std::runtime_error("Choose a supported runner.");g.runner=std::move(value);
    });});
    add(game.runner=="steam"?"Steam app ID":"Game content",game.content.string(),[this,game] {game_field("Game content or Steam app ID",game.content.string(),[](auto& g,const auto& value) {g.content=fs::u8path(value);});});
    if(game.runner=="retroarch")add("Libretro core",game.core.string(),[this,game] {game_field("Libretro core path",game.core.string(),[](auto& g,const auto& value) {g.core=fs::u8path(value);});});
    add("Executable", game.executable.string(), [this, game] { game_field("Executable path or command", game.executable.string(), [](auto &g, auto v) { g.executable = std::move(v); }); });
    add("Launch arguments", desktop::format_arguments(game.arguments), [this, game] { game_field("Arguments (quote values containing spaces)", desktop::format_arguments(game.arguments), [](auto &g, auto v) { g.arguments = desktop::parse_arguments(v); }); });
    add("Working directory", game.working_directory.string(), [this, game] { game_field("Working directory", game.working_directory.string(), [](auto &g, auto v) { g.working_directory = std::move(v); }); });
    add("Description", game.description, [this, game] { game_field("Description", game.description, [](auto &g, auto v) { g.description = std::move(v); }); });
    add("Genre", game.genre, [this, game] { game_field("Genre", game.genre, [](auto &g, auto v) { g.genre = std::move(v); }); });
    add("Developer", game.developer, [this, game] { game_field("Developer", game.developer, [](auto &g, auto v) { g.developer = std::move(v); }); });
    add("Year", std::to_string(game.year), [this, game] { game_field("Release year (0 if unknown)", std::to_string(game.year), [](auto &g, const auto &v) { const auto year = std::stoi(v); if (year < 0 || year > 9999) throw std::runtime_error("Year must be 0-9999"); g.year = year; }); });
    add("Custom category", game.category, [this, game] { game_field("Custom category name", game.category, [](auto &g, auto v) { g.category = std::move(v); }); });
    add(game.favorite ? "Remove favorite" : "Mark favorite", "Favorites are kept in your local profile", [this, game]() mutable { game.favorite = !game.favorite; library_->save_game(game); refresh(); });
    const std::array<std::pair<std::string, fs::path desktop::Game::*>, 4> media{{{"Icon image", &desktop::Game::icon}, {"Background image", &desktop::Game::background}, {"Preview video", &desktop::Game::preview}, {"Preview music", &desktop::Game::music}}};
    for (const auto &[label, member] : media) add(label, (game.*member).string(), [this, game, label, member] { game_field(label + " path", (game.*member).string(), [member](auto &g, auto v) { g.*member = std::move(v); }); }, label == "Icon image" || label == "Background image" ? game.*member : fs::path{});
    if (!game.background.empty()) add("View background artwork", game.background.filename().string(), [this, game] { open_file(game.background); }, game.background);
    if (!game.preview.empty()) add("Play video preview", game.preview.filename().string(), [this, game] { open_file(game.preview); });
    if (!game.music.empty()) add("Play preview music", game.music.filename().string(), [this, game] { if (!openxmb::music::service().set_queue({{game.music, game.title}})) throw std::runtime_error(openxmb::music::service().snapshot().error); message_ = "Playing game preview music"; });
    add("Game Artwork", "Native images, custom files and SteamGridDB", [this,game] {pending_=[game](app::shell* host) {host->show_game_artwork(game);};});
    add("Remove from game library", "Keeps the executable and source media", [this, id = game.id] { confirm("Remove this game entry?", [this, id] { library_->remove_game(id); }); });
    break;
  }
  case Page::roots:
    add("Add game folder", "The scanner reads files; it never launches discovered games", [this] { ask("Game folder path", "", [this](std::string path) { library_->add_game_root(path); }); });
    add("Add detected RPCS3 game folders", "Uses installed RPCS3 data roots; custom disc folders can be added normally", [this] { const auto roots = desktop::rpcs3_game_roots(configured_discovery_options().rpcs3_executable); if (roots.empty()) message_ = "No RPCS3 data folder detected. Add your disc or installed-game folder manually."; else { for (const auto &root : roots) library_->add_game_root(root); message_ = "Added " + std::to_string(roots.size()) + " RPCS3 root(s)."; refresh(); } });
    add("Scan configured folders", "Supported manifest: openxmb-game.json", [this] { message_ = "Added " + std::to_string(library_->scan_games(configured_discovery_options())) + " game(s)."; refresh(); });
    for (const auto &root : library_->game_roots()) add(root.filename().string(), root.string() + "  Select to remove scan root", [this, root] { confirm("Remove this scan root?", [this, root] { library_->remove_game_root(root); }); });
    break;
  case Page::custom:
    add("Add custom section", "Create a named category backed by a local folder", [this] {
      ask("Section name", "My Section", [this](std::string title) { ask("Section folder path", "", [this, title = std::move(title)](std::string folder) { desktop::CustomSection section; section.title = title; section.folder = std::move(folder); library_->save_section(section); menu_dirty_ = true; message_ = "Custom section saved"; }); });
    });
    for (const auto &section : library_->custom_sections()) add(section.title, section.folder.string(), [this, id = section.id] { chosen_id_ = id; enter(Page::section); }, section.icon);
    break;
  case Page::section: {
    auto section = find(library_->custom_sections(), chosen_id_);
    add("Open " + section.title, section.folder.string(), [this, section] { folder_ = section.folder; folder_history_.clear(); enter(Page::folder); }, section.icon);
    add("Rename section", section.title, [this, section] { ask("Section name", section.title, [this, section](std::string value) mutable { section.title = std::move(value); library_->save_section(section); menu_dirty_ = true; }); });
    add("Folder", section.folder.string(), [this, section] { ask("Section folder path", section.folder.string(), [this, section](std::string value) mutable { section.folder = std::move(value); library_->save_section(section); menu_dirty_ = true; }); });
    add("Icon", section.icon.string(), [this, section] { ask("Section icon path", section.icon.string(), [this, section](std::string value) mutable { section.icon = std::move(value); library_->save_section(section); menu_dirty_ = true; }); });
    add("Remove section", "Keeps its folder and files", [this, id = section.id] { confirm("Remove this custom section?", [this, id] { library_->remove_section(id); menu_dirty_ = true; }); });
    break;
  }
  case Page::friends:
    add("Add local friend", "Offline simulation; no messages or remote requests are sent", [this] { ask("Friend display name", "", [this](std::string name) { desktop::Friend person; person.name = std::move(name); library_->save_friend(person); }); });
    for (const auto &person : library_->friends()) add(person.name + (person.present ? " (present locally)" : " (away locally)"), person.status, [this, id = person.id] { chosen_id_ = id; enter(Page::person); });
    break;
  case Page::person: {
    auto person = find(library_->friends(), chosen_id_);
    add("Display name", person.name, [this, person] { ask("Local friend's name", person.name, [this, person](std::string value) mutable { person.name = std::move(value); library_->save_friend(person); }); });
    add("Local status", person.status, [this, person] { ask("Local status text", person.status, [this, person](std::string value) mutable { person.status = std::move(value); library_->save_friend(person); }); });
    add("Toggle simulated presence", person.present ? "Present locally" : "Away locally", [this, person]() mutable { person.present = !person.present; library_->save_friend(person); refresh(); });
    add("Remove local friend", "Removes this offline record", [this, id = person.id] { confirm("Remove local friend?", [this, id] { library_->remove_friend(id); }); });
    break;
  }
  case Page::account: {
    auto account = library_->account();
    add("Display name", account.display_name, [this, account] { ask("Local account display name", account.display_name, [this, account](std::string value) mutable { account.display_name = std::move(value); library_->save_account(account); }); }, account.avatar);
    add("Biography", account.biography, [this, account] { ask("Local biography", account.biography, [this, account](std::string value) mutable { account.biography = std::move(value); library_->save_account(account); }); });
    add("Status", account.status, [this, account] { ask("Local account status", account.status, [this, account](std::string value) mutable { account.status = std::move(value); library_->save_account(account); }); });
    add("Avatar", account.avatar.string(), [this, account] { ask("Avatar image path", account.avatar.string(), [this, account](std::string value) mutable { account.avatar = std::move(value); library_->save_account(account); }); });
    add("Local friends", "Manage offline simulated friend records", [this] { enter(Page::friends); });
    add("Trophy Collection", "RPCS3 trophies and linked achievement progress", [this] { pending_=[](app::shell* host) { host->open_panel("trophies"); }; });
    break;
  }
  case Page::trophies:
    add("Add local trophy", "Offline tracking; does not connect to a console account", [this] { ask("Trophy title", "", [this](std::string title) { desktop::Trophy trophy; trophy.title = std::move(title); library_->save_trophy(trophy); }); });
    for (const auto &trophy : library_->trophies()) add((trophy.unlocked ? "Unlocked: " : "Locked: ") + trophy.title, trophy.grade + "  " + trophy.game, [this, id = trophy.id] { chosen_id_ = id; enter(Page::trophy); });
    break;
  case Page::trophy: {
    auto trophy = find(library_->trophies(), chosen_id_);
    add(trophy.unlocked ? "Mark locked" : "Unlock local trophy", "Only this local profile is changed", [this, trophy]() mutable { trophy.unlocked = !trophy.unlocked; library_->save_trophy(trophy); refresh(); });
    add("Title", trophy.title, [this, trophy] { ask("Trophy title", trophy.title, [this, trophy](std::string value) mutable { trophy.title = std::move(value); library_->save_trophy(trophy); }); });
    add("Game", trophy.game, [this, trophy] { ask("Game title", trophy.game, [this, trophy](std::string value) mutable { trophy.game = std::move(value); library_->save_trophy(trophy); }); });
    add("Description", trophy.description, [this, trophy] { ask("Trophy description", trophy.description, [this, trophy](std::string value) mutable { trophy.description = std::move(value); library_->save_trophy(trophy); }); });
    add("Grade", trophy.grade + "  Select to cycle", [this, trophy]() mutable { const std::array<std::string, 4> grades{"bronze", "silver", "gold", "platinum"}; const auto found = std::ranges::find(grades, trophy.grade); trophy.grade = grades[(static_cast<std::size_t>(found - grades.begin()) + 1) % grades.size()]; library_->save_trophy(trophy); refresh(); });
    add("Remove local trophy", "Keeps all game files", [this, id = trophy.id] { confirm("Remove local trophy?", [this, id] { library_->remove_trophy(id); }); });
    break;
  }
  case Page::folder: {
    std::vector<fs::directory_entry> entries;
    for (const auto &entry : fs::directory_iterator(folder_)) { if (entries.size() >= 2000) break; entries.push_back(entry); }
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { if (a.is_directory() != b.is_directory()) return a.is_directory(); return a.path().filename() < b.path().filename(); });
    for (const auto &entry : entries) add(entry.path().filename().string(), entry.is_directory() ? "Folder" : "Open in native viewer", [this, path = entry.path(), directory = entry.is_directory()] { if (directory) { folder_history_.push_back(folder_); folder_ = path; selected_ = 0; refresh(); } else open_file(path); });
    break;
  }
  case Page::confirm:
    add("Confirm", "Apply this change to local library data", [this] { const auto task = confirm_action_; task(); page_ = root_page_; history_.clear(); selected_ = 0; message_ = "Local library updated"; refresh(); });
    add("Cancel", "Keep current data", [this] { leave(); });
    break;
  }
  if (page_ == Page::game) { const auto game = std::make_shared<const desktop::Game>(find(library_->games(), chosen_id_)); for (auto &row : rows_) row.game = game; }
  if (selected_ >= rows_.size()) selected_ = rows_.empty() ? 0 : rows_.size() - 1;
}
} // namespace programs
