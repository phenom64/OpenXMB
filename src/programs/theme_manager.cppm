module;

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "openxmb/themes/library.hpp"

export module openxmb.app:theme_manager;

import :component;
import :main;
import :text_entry;
import openxmb.utils;
import dreamrender;
import glm;

export namespace programs {

// The manager is a normal native overlay: its entire workflow is usable with
// keyboard or controller, including paths and preset names through the OSK.
class theme_manager : public app::component, public app::event_receiver {
public:
    float typography_scale() const override { return 2.5F; }

  using apply_callback = std::function<void(const openxmb::themes::AppliedTheme&)>;
  using settings_callback = std::function<nlohmann::json()>;
  theme_manager(dreamrender::resource_loader& loader,
                std::shared_ptr<openxmb::themes::ThemeLibrary> library,
                apply_callback apply, settings_callback current_settings = {})
      : loader_(loader), library_(std::move(library)), apply_(std::move(apply)), current_settings_(std::move(current_settings)) { refresh(); }

  result tick(app::shell* shell) override {
    shell_ = shell;
    if (work_.valid() && work_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      try {
        auto result = work_.get(); refresh();
        if (!result.id.empty()) {
          chosen_id_ = result.id;
          for (std::size_t i = 0; i < rows_.size(); ++i) if (rows_[i].kind == Kind::theme && rows_[i].value == result.id) { selected_ = i; break; }
        }
        message_ = std::move(result.message);
      } catch (const std::exception& error) { message_ = error.what(); }
    }
    return result::success;
  }

  result on_event(const app::event& event) override {
    if (work_.valid()) { message_ = "Theme operation in progress. Originals remain unchanged."; return result::success; }
    if (event.action == action::cancel) {
      if (components_page_) { components_page_ = false; selected_ = 0; return result::success | result::back_sound; }
      return result::close | result::back_sound;
    }
    if (event.action == action::left || event.action == action::right) {
      components_page_ = !components_page_; selected_ = 0;
      return result::success;
    }
    const auto count = components_page_ ? 6U : rows_.size();
    if (event.action == action::up && count) selected_ = (selected_ + count - 1) % count;
    if (event.action == action::down && count) selected_ = (selected_ + 1) % count;
    if (event.action == action::extra && !components_page_) { refresh(); message_ = "Theme library refreshed."; }
    if (event.action == action::options && !components_page_ && selected_ < rows_.size() && rows_[selected_].kind == Kind::preset) {
      const auto name = rows_[selected_].value;
      text("Delete preset? Type DELETE to confirm", "", [this, name](std::string confirmation) {
        if (confirmation != "DELETE" && confirmation != "delete") { message_ = "Preset kept."; return; }
        attempt([&] { library_->delete_preset(name); refresh(); message_ = "Preset deleted."; });
      });
      return result::success;
    }
    if (event.action != action::ok) return result::success;
    try {
      if (components_page_) {
        bool* options[] = {&options_.background, &options_.icons, &options_.sounds, &options_.colors, &options_.font, &options_.overlay};
        *options[selected_] = !*options[selected_];
        return result::success | result::ok_sound;
      }
      const auto row = rows_.at(selected_);
      if (row.kind == Kind::theme) {
        chosen_id_ = row.value;
        apply_(library_->apply(chosen_id_, options_));
        message_ = "Theme applied. Your component choices were saved.";
      } else if (row.kind == Kind::defaults) {
        apply_(library_->revert());
        message_ = "Original appearance restored.";
      } else if (row.kind == Kind::import_path) {
        text("Import .p3t, .zip or theme folder", library_->inbox().string(), [this](std::string path) {
          start_work("Importing theme...", [library = library_, path = std::move(path)] {
            auto theme = library->import_theme(path); return WorkResult{theme.id, "Imported " + theme.name + ". Select it to apply."};
          });
        });
      } else if (row.kind == Kind::create_theme) {
        text("Create Custom Theme", "My Theme", [this](std::string name) {
          auto settings = current_settings_ ? current_settings_() : library_->active().value_or(openxmb::themes::AppliedTheme{}).shell;
          start_work("Creating custom theme...", [library = library_, name = std::move(name), settings = std::move(settings)]() mutable {
            auto theme = library->create_custom_theme(name, std::move(settings));
            return WorkResult{theme.id, "Custom theme created. Export it as a ZIP to share."};
          });
        });
      } else if (row.kind == Kind::inbox) {
        start_work("Importing inbox...", [library = library_, files = inbox_] {
          std::size_t imported = 0; std::string id, errors;
          for (const auto& path : files) try { auto theme = library->import_theme(path); id = theme.id; ++imported; }
            catch (const std::exception& error) { errors = " Last error: " + std::string(error.what()); }
          return WorkResult{id, "Imported " + std::to_string(imported) + " theme(s)." + errors};
        });
      } else if (row.kind == Kind::export_theme) {
        require_chosen();
        text("Export theme ZIP (new filename)", (library_->root() / "exports" / (chosen_id_ + ".zip")).string(), [this](std::string path) {
          start_work("Exporting theme...", [library = library_, id = chosen_id_, path = std::move(path)] {
            library->export_theme(id, path); return WorkResult{id, "Theme exported to " + path};
          });
        });
      } else if (row.kind == Kind::save_preset) {
        require_chosen();
        text("Save component preset", "My Theme", [this](std::string name) {
          attempt([&] { library_->save_preset(name, chosen_id_, options_); refresh(); message_ = "Preset saved."; });
        });
      } else if (row.kind == Kind::remove_theme) {
        require_chosen();
        text("Remove installed theme? Type DELETE to confirm", "", [this, id = chosen_id_](std::string confirmation) {
          if (confirmation != "DELETE" && confirmation != "delete") { message_ = "Theme kept."; return; }
          attempt([&] { library_->remove_theme(id); chosen_id_.clear(); refresh(); message_ = "Installed copy removed. Original import source was kept."; });
        });
      } else if (row.kind == Kind::preset) {
        auto applied = library_->load_preset(row.value); chosen_id_ = applied.id; options_ = applied.components;
        apply_(applied); message_ = "Preset applied.";
      }
      return result::success | result::ok_sound;
    } catch (const std::exception& error) { message_ = error.what(); return result::success | result::error_rumble; }
  }

  void render(dreamrender::gui_renderer& renderer, app::shell* shell) override {
    shell_ = shell;
    renderer.draw_rect({0.045F, 0.075F}, {0.91F, 0.83F}, {0.018F, 0.025F, 0.04F, 0.96F});
    renderer.draw_text("Theme Library", 0.085F, 0.14F, 0.05F);
    renderer.draw_text(components_page_ ? "Components     < Library" : "Installed Themes     > Components", 0.085F, 0.205F, 0.029F, {0.62F, 0.78F, 0.95F, 1.0F});
    if (components_page_) {
      const std::array<std::pair<std::string_view, bool>, 6> options{{{"Background / animation", options_.background}, {"Icons", options_.icons},
          {"Sound effects", options_.sounds}, {"Theme colors", options_.colors}, {"Font", options_.font}, {"Overlay layers", options_.overlay}}};
      for (unsigned i = 0; i < options.size(); ++i)
        render_row(renderer, std::string(options[i].first) + (options[i].second ? "   On" : "   Off"), i, i, selected_ == i);
      renderer.draw_text("Choose the parts to use, then return to the Library to apply.", 0.085F, 0.76F, 0.025F);
    } else {
      constexpr std::size_t visible = 8;
      const auto first = selected_ >= visible ? selected_ - visible + 1 : 0;
      for (std::size_t i = first; i < std::min(first + visible, rows_.size()); ++i)
        render_row(renderer, rows_[i].label, i - first, i, selected_ == i);
      std::optional<openxmb::themes::ThemeInfo> preview;
      if (selected_ < rows_.size() && rows_[selected_].kind == Kind::theme) {
        try { preview = library_->preview(rows_[selected_].value); } catch (...) {}
      } else if (!chosen_id_.empty()) { try { preview = library_->preview(chosen_id_); } catch (...) {} }
      if (preview) {
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        const auto frame = openxmb::themes::ThemeLibrary::sample_animation(*preview, seconds);
        auto image = frame.background.empty() ? preview->preview : frame.background;
        draw_image(renderer, image, 0.57F, 0.28F, 0.31F, 0.28F);
        for (const auto& layer : frame.layers) draw_image(renderer, layer.image, 0.57F + layer.x * 0.31F, 0.28F + layer.y * 0.28F,
            layer.width * 0.31F, layer.height * 0.28F, layer.opacity);
        for (const auto& draw : frame.draws) {
          const auto texture = preview_texture(draw.image);
          if (!texture || !texture->loaded) continue;
          std::vector<dreamrender::image_renderer::textured_vertex> vertices;
          vertices.reserve(draw.vertices.size());
          for (const auto& vertex : draw.vertices)
            vertices.push_back({{0.57F + vertex.x * 0.31F, 0.28F + vertex.y * 0.28F, vertex.z, vertex.w},
                {vertex.u, vertex.v}, {vertex.color[0], vertex.color[1], vertex.color[2], vertex.color[3]}});
          renderer.draw_textured_triangles(*texture, vertices, draw.additive, draw.depth);
        }
        renderer.draw_text(preview->name, 0.57F, 0.60F, 0.031F);
        if (!preview->author.empty()) renderer.draw_text(preview->author, 0.57F, 0.645F, 0.025F);
        const auto details = std::to_string(preview->icons.size()) + " icons  " + std::to_string(preview->sounds.size()) + " sounds" + (preview->dynamic ? "  Animated" : "");
        renderer.draw_text(details, 0.57F, 0.69F, 0.023F);
        if (!frame.warnings.empty()) renderer.draw_text(shorten(frame.warnings.front(), 62), 0.57F, 0.735F, 0.021F, {1.0F, 0.76F, 0.48F, 1.0F});
      } else {
        renderer.draw_text("Import .p3t / .zip themes or icon folders.", 0.57F, 0.34F, 0.025F);
        renderer.draw_text("Inbox: " + shorten(library_->inbox().string(), 48), 0.57F, 0.40F, 0.022F);
        renderer.draw_text("Theme originals are left unchanged.", 0.57F, 0.46F, 0.022F);
      }
    }
    renderer.draw_text(shorten(message_, 110), 0.085F, 0.82F, 0.024F);
    render_controller_buttons(shell, renderer, 0.5F, 0.87F, std::array{
      std::pair{action::ok, std::string_view{"Select / Apply"}}, std::pair{action::left, std::string_view{"Components"}},
      std::pair{action::extra, std::string_view{"Refresh"}}, std::pair{action::cancel, std::string_view{"Back"}}
    });
  }
  bool is_opaque() const override { return false; }

private:
  enum class Kind { defaults, theme, import_path, create_theme, inbox, export_theme, save_preset, remove_theme, preset };
  struct Row { std::string label; Kind kind; std::string value; };
  struct WorkResult { std::string id, message; };
  static std::string shorten(const std::string& text, std::size_t maximum) { return text.size() > maximum ? text.substr(0, maximum - 3) + "..." : text; }
  void refresh() {
    rows_.clear(); rows_.push_back({"Restore original appearance", Kind::defaults, {}});
    for (const auto& theme : library_->discover()) rows_.push_back({theme.name, Kind::theme, theme.id});
    rows_.push_back({"Import theme or icon pack...", Kind::import_path, {}});
    rows_.push_back({"Create Custom Theme...", Kind::create_theme, {}});
    inbox_.clear();
    for (const auto& entry : std::filesystem::directory_iterator(library_->inbox()))
      if (!entry.is_symlink() && (entry.is_directory() || entry.path().extension() == ".p3t" || entry.path().extension() == ".zip")) inbox_.push_back(entry.path());
    rows_.push_back({"Import inbox (" + std::to_string(inbox_.size()) + ")", Kind::inbox, {}});
    rows_.push_back({"Export selected theme...", Kind::export_theme, {}});
    rows_.push_back({"Save component preset...", Kind::save_preset, {}});
    rows_.push_back({"Remove installed theme...", Kind::remove_theme, {}});
    for (const auto& preset : library_->presets()) rows_.push_back({"Preset: " + preset, Kind::preset, preset});
    selected_ = std::min(selected_, rows_.size() - 1);
    if (auto active = library_->active()) { chosen_id_ = active->id; options_ = active->components; }
  }
  void text(const std::string& title, const std::string& initial, std::function<void(std::string)> callback) {
    if (!shell_) throw openxmb::themes::ThemeError("Theme manager is not attached to the shell yet");
    shell_->emplace_overlay<app::text_entry_overlay>(title, initial, std::move(callback));
  }
  template<class Function> void attempt(Function&& operation) { try { operation(); } catch (const std::exception& error) { message_ = error.what(); } }
  template<class Function> void start_work(std::string message, Function&& operation) {
    message_ = std::move(message);
    work_ = std::async(std::launch::async, std::forward<Function>(operation));
  }
  void require_chosen() { if (chosen_id_.empty()) throw openxmb::themes::ThemeError("Apply a theme first, then use this action."); }
  void render_row(dreamrender::gui_renderer& renderer, const std::string& label, std::size_t row, std::size_t, bool selected) {
    const auto y = 0.28F + static_cast<float>(row) * 0.057F;
    if (selected) renderer.draw_rect({0.075F, y - 0.023F}, {0.445F, 0.050F}, {0.24F, 0.40F, 0.62F, 0.75F});
    renderer.draw_text(shorten(label, 48), 0.095F, y, 0.030F);
  }
  struct PreviewTexture { std::shared_ptr<dreamrender::texture> texture; std::shared_future<void> ready; std::uint64_t used = 0; };
  std::shared_ptr<dreamrender::texture> preview_texture(const std::filesystem::path& path) {
    if (path.empty()) return {};
    if (!textures_.contains(path) && textures_.size() >= 8) {
      auto oldest = textures_.end();
      for (auto it = textures_.begin(); it != textures_.end(); ++it)
        if (it->second.ready.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
            (oldest == textures_.end() || it->second.used < oldest->second.used)) oldest = it;
      if (oldest == textures_.end()) return {};
      textures_.erase(oldest);
    }
    auto& item = textures_[path];
    if (!item.texture) {
      item.texture = std::make_shared<dreamrender::texture>(loader_.getDevice(), loader_.getAllocator());
      item.ready = loader_.loadTexture(item.texture.get(), path);
    }
    item.used = ++texture_use_;
    return item.texture;
  }
  void draw_image(dreamrender::gui_renderer& renderer, const std::filesystem::path& path, float x, float y, float width, float height, float opacity = 1.0F) {
    const auto texture = preview_texture(path);
    if (texture && texture->loaded) renderer.draw_image(texture->imageView.get(), x, y, width * renderer.aspect_ratio, height, {1.0F, 1.0F, 1.0F, opacity});
  }
  dreamrender::resource_loader& loader_;
  std::shared_ptr<openxmb::themes::ThemeLibrary> library_;
  apply_callback apply_;
  settings_callback current_settings_;
  app::shell* shell_ = nullptr;
  std::vector<Row> rows_;
  std::vector<std::filesystem::path> inbox_;
  std::map<std::filesystem::path, PreviewTexture> textures_;
  std::uint64_t texture_use_ = 0;
  std::future<WorkResult> work_;
  openxmb::themes::ComponentOptions options_;
  std::string chosen_id_, message_ = "Select a theme to preview; confirm to apply.";
  std::size_t selected_ = 0;
  bool components_page_ = false;
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};
} // namespace programs
