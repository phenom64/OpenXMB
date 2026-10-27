module;

#include "openxmb/desktop/library.hpp"
#include "openxmb/music/player.hpp"
#include "openxmb/desktop/preview_audio.hpp"
#if OPENXMB_ENABLE_VIDEO_PLAYER
#include "openxmb/video_worker.hpp"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <tuple>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module openxmb.app;

import :library_manager;
import :main;
import :text_entry;
import :programs;
import glm;
import auroreengine;
import vulkan_hpp;
import vma;
import openxmb.utils;

namespace programs {
namespace desktop = openxmb::desktop;
namespace fs = std::filesystem;
namespace {
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
struct library_manager::Preview {
  auroreengine::resource_loader &loader;
  vk::Device device;
  vma::Allocator allocator;
  auroreengine::window *window{};
  std::shared_ptr<const desktop::Game> selected;
  std::string selection_key;
  std::chrono::steady_clock::time_point selected_at{};
  std::shared_ptr<auroreengine::texture> background;
  std::future<void> background_ready;
  fs::path background_path;
  desktop::PreviewAudio audio;
  bool started{}, owns_duck{};
#if OPENXMB_ENABLE_VIDEO_PLAYER
  openxmb::video::worker movie;
  std::optional<openxmb::video::frame> pending;
  std::unique_ptr<auroreengine::texture> movie_texture;
  std::vector<vma::UniqueAllocation> allocations;
  std::vector<vma::UniqueBuffer> buffers;
  bool movie_initialized{}, movie_current{};
  double movie_aspect{16.0 / 9.0};
#endif
  explicit Preview(auroreengine::resource_loader &value)
      : loader(value), device(value.getDevice()), allocator(value.getAllocator()) {}
  ~Preview() {
    stop();
    if (background_ready.valid()) background_ready.wait();
    try { wait_gpu(); } catch (...) {}
  }
  void wait_gpu() {
    if (window) window->waitIdle(); else device.waitIdle();
  }
  void duck(bool enabled) {
    if (enabled) { openxmb::music::service().set_external_duck(true); owns_duck = true; }
    else if (owns_duck) { openxmb::music::service().set_external_duck(false); owns_duck = false; }
  }
  void stop() {
    audio.stop(); duck(false);
#if OPENXMB_ENABLE_VIDEO_PLAYER
    movie.set_path({}); pending.reset(); movie_current = false;
#endif
    selected.reset(); selection_key.clear(); started = false;
  }
  void choose(std::shared_ptr<const desktop::Game> game) {
    const std::string key = game ? game->id + '\n' + game->background.string() + '\n' + game->preview.string() + '\n' + game->music.string() : "";
    if (key == selection_key) return;
    stop(); selected = std::move(game); selection_key = key;
    selected_at = std::chrono::steady_clock::now();
    if (!selected) return;
    if (selected->background != background_path) {
      if (background_ready.valid()) background_ready.wait();
      if (background) wait_gpu();
      background.reset(); background_path = selected->background;
      std::error_code error;
      if (!background_path.empty() && fs::is_regular_file(background_path, error)) {
        background = std::make_shared<auroreengine::texture>(device, allocator);
        background_ready = loader.loadTexture(background.get(), background_path);
      }
    }
  }
  void tick(auroreengine::window *value) {
    window = value;
    if (!selected) return;
    if (!started && std::chrono::steady_clock::now() - selected_at >= std::chrono::milliseconds(350)) {
      started = true;
      if (!selected->music.empty()) audio.set_path(selected->music, 0.45f);
#if OPENXMB_ENABLE_VIDEO_PLAYER
      if (!selected->preview.empty()) {
        openxmb::video::options options;
        options.hardware_acceleration = "off";
        options.audio_output = selected->music.empty();
        movie.set_path(selected->preview, options);
        movie.set_repeat(true);
        movie.set_volume(0.45f);
      }
#endif
    }
    audio.tick();
    bool audible = audio.active();
#if OPENXMB_ENABLE_VIDEO_PLAYER
    if (started && !selected->preview.empty()) {
      const auto snapshot = movie.snapshot();
      if (snapshot->ready && snapshot->status != openxmb::video::state::failed) {
        if (auto frame = movie.take_frame()) pending = std::move(frame);
        audible = audible || (selected->music.empty() && !snapshot->info.audio_tracks.empty());
      }
    }
#endif
    duck(audible);
  }
  void upload(vk::CommandBuffer command, int frame, auroreengine::window *value) {
    window = value;
#if OPENXMB_ENABLE_VIDEO_PLAYER
    if (!pending) return;
    const auto &image = *pending;
    if (!movie_texture || movie_texture->width != image.width || movie_texture->height != image.height || buffers.size() != window->swapchainImageCount) {
      wait_gpu(); movie_texture.reset(); buffers.clear(); allocations.clear();
      movie_texture = std::make_unique<auroreengine::texture>(device, allocator, image.width, image.height);
      buffers.resize(window->swapchainImageCount); allocations.resize(window->swapchainImageCount);
      const vk::BufferCreateInfo info({}, image.rgba.size(), vk::BufferUsageFlagBits::eTransferSrc, vk::SharingMode::eExclusive);
      const vma::AllocationCreateInfo memory({}, vma::MemoryUsage::eCpuToGpu);
      for (std::size_t i = 0; i < buffers.size(); ++i) std::tie(buffers[i], allocations[i]) = allocator.createBufferUnique(info, memory);
      movie_initialized = false;
    }
    if (frame < 0 || static_cast<std::size_t>(frame) >= buffers.size()) return;
    allocator.copyMemoryToAllocation(image.rgba.data(), allocations[frame].get(), 0, image.rgba.size());
    const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
    command.pipelineBarrier(movie_initialized ? vk::PipelineStageFlagBits::eFragmentShader : vk::PipelineStageFlagBits::eTopOfPipe,
      vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier(movie_initialized ? vk::AccessFlags{vk::AccessFlagBits::eShaderRead} : vk::AccessFlags{},
        vk::AccessFlagBits::eTransferWrite, movie_initialized ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
        vk::ImageLayout::eTransferDstOptimal, vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, movie_texture->image, range));
    command.copyBufferToImage(buffers[frame].get(), movie_texture->image, vk::ImageLayout::eTransferDstOptimal,
      vk::BufferImageCopy(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1), vk::Offset3D(0, 0, 0),
        vk::Extent3D(static_cast<std::uint32_t>(image.width), static_cast<std::uint32_t>(image.height), 1)));
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {},
      vk::ImageMemoryBarrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead, vk::ImageLayout::eTransferDstOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal, vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, movie_texture->image, range));
    movie_initialized = movie_current = true; movie_aspect = image.aspect_ratio; pending.reset();
#else
    (void)command; (void)frame;
#endif
  }
  bool has_movie() const {
#if OPENXMB_ENABLE_VIDEO_PLAYER
    return selected && movie_texture && movie_current;
#else
    return false;
#endif
  }
  void draw_background(auroreengine::gui_renderer &r) {
    if (selected && background && background->loaded)
      r.draw_image(background->imageView.get(), 0.0f, 0.0f, static_cast<float>(r.aspect_ratio), 1.0f, {1.0f, 1.0f, 1.0f, 0.80f});
  }
  void draw_movie(auroreengine::gui_renderer &r) {
#if OPENXMB_ENABLE_VIDEO_PLAYER
    if (!has_movie()) return;
    const float width = 0.25f * static_cast<float>(r.aspect_ratio);
    const float height = std::min(0.33f, width / static_cast<float>(std::max(0.1, movie_aspect)));
    r.draw_rect({0.665f, 0.275f}, {0.26f, height + 0.02f}, {0.0f, 0.0f, 0.0f, 0.65f});
    r.draw_image(movie_texture->imageView.get(), 0.67f, 0.285f, width, height);
#else
    (void)r;
#endif
  }
};

library_manager::library_manager(std::string section,
    auroreengine::resource_loader &loader, profile_callback callback)
    : preview_(std::make_unique<Preview>(loader)), loader_(loader), switch_profile_(std::move(callback)) {
  if (section == "users" || section == "profiles") page_ = Page::users;
  else if (section == "custom" || section == "sections") page_ = Page::custom;
  else if (section == "friends") page_ = Page::friends;
  else if (section == "account" || section == "network") page_ = Page::account;
  else if (section == "trophies") page_ = Page::trophies;
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
    if (menu_dirty_) shell->reload_menu();
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
  case Page::custom: return "Custom sections"; case Page::section: return "Custom section";
  case Page::friends: return "Friends - local simulation"; case Page::person: return "Local friend";
  case Page::account: return "Local account - no online sign-in"; case Page::trophies: return "Local trophies";
  case Page::trophy: return "Local trophy"; case Page::folder: return folder_.filename().string(); case Page::confirm: return confirm_title_;
  }
  return "Library";
}
void library_manager::render(auroreengine::gui_renderer &r, app::shell *shell) {
  preview_->draw_background(r);
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
  case Page::games:
    add("Add game manually", "Choose executable, arguments, artwork, preview video and music", [this] { desktop::Game value; value.title = "New Game"; edit_game(library_->save_game(std::move(value)).id); });
    add("Manage game folders", "Configure scan roots without moving your games", [this] { enter(Page::roots); });
    add("Scan game folders", "Imports game manifests, desktop entries and native executables", [this] { message_ = "Added " + std::to_string(library_->scan_games()) + " game(s)."; refresh(); });
    for (const auto &game : library_->games()) add((game.favorite ? "* " : "") + game.title, game.genre + (game.year ? "  " + std::to_string(game.year) : "") + "  Played " + std::to_string(game.play_count) + " times", [this, id = game.id] { edit_game(id); }, game.icon.empty() ? game.background : game.icon, std::make_shared<const desktop::Game>(game));
    break;
  case Page::game: {
    auto game = find(library_->games(), chosen_id_);
    add("Launch " + game.title, game.executable.string(), [this, game] { preview_suspended_ = true; preview_->stop(); desktop::launch_game(game); library_->mark_played(game.id); message_ = "Started " + game.title; refresh(); }, game.icon);
    add("Title", game.title, [this, game] { game_field("Game title", game.title, [](auto &g, auto v) { g.title = std::move(v); }); });
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
    add("Optional RAWG artwork provider", library_->artwork_provider_available() ? "Enabled and key is configured" : "Disabled or key absent; local artwork remains available", [this] {
      ask("Artwork key environment variable (no key values)", "RAWG_API_KEY", [this](std::string variable) { library_->set_artwork_provider(true, std::move(variable)); message_ = "Provider enabled. Supply the key securely in environment settings."; });
    });
    add("Download artwork for this game", "Makes HTTPS requests only when provider and key are configured", [this, id = game.id] {
      if (artwork_download_.valid()) throw std::runtime_error("An artwork request is already in progress");
      if (!library_->artwork_provider_available()) throw std::runtime_error("Enable the provider and configure its key first; no request was made");
      const auto root = library_->root(); artwork_download_ = std::async(std::launch::async, [root, id] { desktop::Library value(root); return value.fetch_artwork(id); }); message_ = "Looking up game artwork...";
    });
    add("Disable artwork provider", "Stops optional provider lookups", [this] { library_->set_artwork_provider(false); message_ = "Artwork provider disabled"; refresh(); });
    add("Remove from game library", "Keeps the executable and source media", [this, id = game.id] { confirm("Remove this game entry?", [this, id] { library_->remove_game(id); }); });
    break;
  }
  case Page::roots:
    add("Add game folder", "The scanner reads files; it never launches discovered games", [this] { ask("Game folder path", "", [this](std::string path) { library_->add_game_root(path); }); });
    add("Add detected RPCS3 game folders", "Uses installed RPCS3 data roots; custom disc folders can be added normally", [this] { const auto roots = desktop::rpcs3_game_roots(); if (roots.empty()) message_ = "No RPCS3 data folder detected. Add your disc or installed-game folder manually."; else { for (const auto &root : roots) library_->add_game_root(root); message_ = "Added " + std::to_string(roots.size()) + " RPCS3 root(s)."; refresh(); } });
    add("Scan configured folders", "Supported manifest: openxmb-game.json", [this] { message_ = "Added " + std::to_string(library_->scan_games()) + " game(s)."; refresh(); });
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
    add("Local trophies", "Manage offline trophy records", [this] { enter(Page::trophies); });
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
