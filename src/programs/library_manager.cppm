module;

#include "openxmb/desktop/library.hpp"

#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

export module openxmb.app:library_manager;

import :component;
import :game_presentation;
import openxmb.utils;
import auroreengine;
import vulkan_hpp;

export namespace programs {
class library_manager final : public app::component, public app::event_receiver {
public:
    float typography_scale() const override { return 2.5F; }

  using profile_callback = std::function<void(std::filesystem::path)>;
  library_manager(std::string section, auroreengine::resource_loader &loader,
                  profile_callback switch_profile = {});
  ~library_manager() override;
  result tick(app::shell *shell) override;
  void prerender(vk::CommandBuffer command, int frame, app::shell *shell) override;
  result on_event(const app::event &event) override;
  void render(auroreengine::gui_renderer &renderer, app::shell *shell) override;
  bool is_opaque() const override { return false; }

private:
  enum class Page { users, profile, games, game, roots, runners, custom, section, friends,
                    person, account, trophies, trophy, folder, confirm };
  struct Row {
    std::string title, detail;
    std::function<void()> activate;
    std::filesystem::path artwork;
    std::shared_ptr<const openxmb::desktop::Game> game;
  };
  void refresh();
  void enter(Page page);
  void leave();
  void ask(std::string title, std::string initial,
           std::function<void(std::string)> accept);
  void confirm(std::string title, std::function<void()> accept);
  void open_file(std::filesystem::path file);
  void edit_game(std::string id);
  void game_field(std::string title, std::string value,
                  std::function<void(openxmb::desktop::Game &, std::string)> set);
  std::string heading() const;
  void attempt(std::function<void()> action);
  std::unique_ptr<app::game_presentation> preview_;
  bool preview_suspended_{}, closing_{}, menu_dirty_{};
  auroreengine::resource_loader &loader_;
  profile_callback switch_profile_;
  std::unique_ptr<openxmb::desktop::Library> library_;
  Page page_{Page::games}, root_page_{Page::games};
  std::vector<Page> history_;
  std::vector<Row> rows_;
  std::size_t selected_{};
  std::string chosen_id_, message_, confirm_title_;
  std::filesystem::path folder_;
  std::vector<std::filesystem::path> folder_history_;
  std::function<void()> confirm_action_;
  std::function<void(app::shell *)> pending_;
  std::future<std::filesystem::path> artwork_download_;
  std::filesystem::path loaded_artwork_;
  std::shared_ptr<auroreengine::texture> artwork_;
  std::future<void> artwork_ready_;
};
} // namespace programs
