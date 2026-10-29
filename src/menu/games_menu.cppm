module;
#include "openxmb/desktop/library.hpp"
#include "../localization/shell_locale.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>
#include <vector>

export module openxmb.app:games_menu;
import :menu_base;
import :main;
import :choice_overlay;
import :message_overlay;
import auroreengine;
import openxmb.config;
import openxmb.utils;

namespace menu {
namespace desktop = openxmb::desktop;
namespace fs = std::filesystem;
namespace {
std::string tr(std::string_view value) { return openxmb::localization::shell_locale::translate(value); }

// Only prepare() may release textures. The shell calls it before recording a
// frame; get_icon() only queues a request and returns an already-live texture.
class game_icon_cache {
  struct request {fs::path icon,background;std::uint64_t touched{};};
  struct item {
    std::shared_ptr<auroreengine::texture> texture;
    std::future<void> ready;
    fs::path icon,background;
    std::uint64_t touched{};
    bool icon_tried{},background_tried{},loading_background{},published{};
  };
  static constexpr std::size_t max_textures=20;
  static constexpr std::size_t max_requests=40;
  static constexpr std::uint64_t max_bytes=64ull*1024*1024;
  auroreengine::resource_loader& loader_;
  app::shell* host_;
  std::unique_ptr<auroreengine::texture> fallback_;
  std::map<std::string,item,std::less<>> items_;
  std::map<std::string,request,std::less<>> requests_;
  std::uint64_t sequence_{};
public:
  game_icon_cache(auroreengine::resource_loader& loader,app::shell* host)
      :loader_(loader),host_(host),fallback_(std::make_unique<auroreengine::texture>(loader.getDevice(),loader.getAllocator())) {
    try {
      // draw_image_a reads dimensions before checking loaded. Wait for this
      // one shared fallback to initialize rather than exposing empty metadata.
      loader_.loadTexture(fallback_.get(),config::CONFIG.asset_directory/"icons/theme/icon_game.png").get();
      if(!fallback_->loaded) throw std::runtime_error("Fallback icon did not load");
    } catch(...) {
      // Keep known dimensions even when the fallback asset is unavailable;
      // its unloaded flag makes the renderer omit the empty texture.
      fallback_=std::make_unique<auroreengine::texture>(loader.getDevice(),loader.getAllocator(),1,1);
    }
  }
  ~game_icon_cache() {
    for(auto& [key,value]:items_) if(value.ready.valid()) value.ready.wait();
    try {if(host_&&host_->get_window()) host_->get_window()->waitIdle();else loader_.getDevice().waitIdle();} catch(...) {}
  }
  const auroreengine::texture& icon(const desktop::Game& game) {
    auto it=items_.find(game.id);
    if(it!=items_.end()) {
      it->second.touched=++sequence_;
      if(it->second.published&&it->second.texture&&it->second.texture->loaded) return *it->second.texture;
      return *fallback_;
    }
    if(!game.icon.empty()||!game.background.empty()) {
      auto [request_it,inserted]=requests_.try_emplace(game.id);
      if(inserted) {request_it->second.icon=game.icon;request_it->second.background=game.background;}
      request_it->second.touched=++sequence_;
      if(requests_.size()>max_requests) {
        auto oldest=std::min_element(requests_.begin(),requests_.end(),
          [](const auto& a,const auto& b){return a.second.touched<b.second.touched;});
        if(oldest!=requests_.end())requests_.erase(oldest);
      }
    }
    return *fallback_;
  }
  void request_selected(const desktop::Game& game) {static_cast<void>(icon(game));}
  void prepare(std::string_view selected) {
    // Resolve futures only on the render thread. Invalid/portrait backgrounds
    // cannot replace ICON0 and quietly fall back to the generic game icon.
    for(auto& [id,value]:items_) {
      if(value.ready.valid()&&value.ready.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try {value.ready.get();
          const auto size=value.texture&&value.texture->loaded
            ? static_cast<std::uint64_t>(std::max(0,value.texture->width))*static_cast<std::uint64_t>(std::max(0,value.texture->height))*4 : 0;
          value.published=size>0&&size<=max_bytes &&
            (!value.loading_background||value.texture->width>value.texture->height);
          if(!value.published)value.texture.reset();
        } catch(...) {value.published=false;}
      }
    }
    auto bytes=[&] {
      std::uint64_t total=0;
      for(const auto& [id,value]:items_) if(value.texture&&value.texture->loaded)
        total+=static_cast<std::uint64_t>(std::max(0,value.texture->width))*static_cast<std::uint64_t>(std::max(0,value.texture->height))*4;
      return total;
    };
    const auto retire=[&](auto it) {
      if(host_&&host_->get_window())host_->get_window()->waitIdle();else loader_.getDevice().waitIdle();
      items_.erase(it);
    };
    // Pre-recording only: all old GPU references are quiesced before erase.
    while(bytes()>max_bytes&&!items_.empty()) {
      auto victim=items_.end();
      for(auto it=items_.begin();it!=items_.end();++it)
        if(!it->second.ready.valid() && it->first!=selected &&
           (victim==items_.end()||it->second.touched<victim->second.touched)) victim=it;
      if(victim==items_.end()) break;
      retire(victim);
    }
    if(!requests_.empty()) {
      auto wanted=requests_.find(selected);
      if(wanted==requests_.end()) wanted=std::max_element(requests_.begin(),requests_.end(),
        [](const auto& a,const auto& b){return a.second.touched<b.second.touched;});
      if(items_.size()>=max_textures) {
        auto victim=items_.end();
        for(auto it=items_.begin();it!=items_.end();++it)
          if(!it->second.ready.valid()&&it->first!=selected&&
             (victim==items_.end()||it->second.touched<victim->second.touched)) victim=it;
        if(victim!=items_.end()) retire(victim);
      }
      if(items_.size()<max_textures) {
        item value;value.icon=wanted->second.icon;value.background=wanted->second.background;value.touched=wanted->second.touched;
        items_.try_emplace(wanted->first,std::move(value));requests_.erase(wanted);
      }
    }
    // Issue at most one request per tick; loader decoding and upload remain
    // asynchronous and a failed path is tried only once per cache residency.
    auto pending=items_.find(selected);
    const auto needs=[](const item& value) {return !value.published&&!value.ready.valid()&&
      ((!value.icon_tried&&!value.icon.empty())||(!value.background_tried&&!value.background.empty()));};
    if(pending==items_.end()||!needs(pending->second)) {
      pending=items_.end();
      for(auto it=items_.begin();it!=items_.end();++it)
        if(needs(it->second)&&(pending==items_.end()||it->second.touched>pending->second.touched))pending=it;
    }
    if(pending!=items_.end()) {
      auto& value=pending->second;
      const bool bg=value.icon_tried||value.icon.empty();
      const auto path=bg?value.background:value.icon;
      if(bg)value.background_tried=true;else value.icon_tried=true;
      value.loading_background=bg;
      // A failed or rejected texture has never been presented, and its
      // loader future is complete before it is replaced.
      value.texture.reset();
      try {
        value.texture=std::make_shared<auroreengine::texture>(loader_.getDevice(),loader_.getAllocator());
        value.ready=loader_.loadTexture(value.texture.get(),path);
      } catch(...) {value.texture.reset();}
    }
  }
};

class game_entry final : public menu_entry {
  std::shared_ptr<const desktop::Game> game_;
  std::shared_ptr<game_icon_cache> cache_;
  app::shell* host_;
  fs::path origin_root_;
  std::string origin_profile_;
public:
  game_entry(std::shared_ptr<const desktop::Game> game,std::shared_ptr<game_icon_cache> cache,
             app::shell* host,fs::path root,std::string profile)
      :game_(std::move(game)),cache_(std::move(cache)),host_(host),origin_root_(std::move(root)),origin_profile_(std::move(profile)) {}
  std::string_view get_name() const override {return game_->title;}
  std::string_view get_description() const override {return game_->title_id;}
  const auroreengine::texture& get_icon() const override {
    if(host_)if(const auto* animated=host_->game_preview_icon(game_->id))return *animated;
    return cache_->icon(*game_);
  }
  const auroreengine::texture* get_glass_icon() const override {return nullptr;}
  bool preserve_icon_colour() const override {return true;}
  result activate(action input) override {
    if(!host_) return result::failure;
    if(input==action::ok) return host_->launch_game(*game_);
    if(input!=action::options) return result::unsupported;
    const auto game=game_;const auto root=origin_root_;const auto profile=origin_profile_;auto* host=host_;
    std::vector<std::string> options{tr("Launch"),tr("Information"),tr(game->favorite?"Remove from Favourites":"Add to Favourites")};
    host->emplace_overlay<app::choice_overlay>(std::move(options),0,[game,root,profile,host](unsigned int index) {
      if(index==0) {static_cast<void>(host->launch_game(*game));return;}
      if(index==1) {host->show_game_information(*game);return;}
      if(index!=2) return;
      try {
        desktop::Library library;
        if(root.empty()||profile.empty()||library.root().lexically_normal()!=root.lexically_normal()||library.active_profile().id!=profile)
          throw std::runtime_error("The active profile changed. Reopen the game library and try again.");
        auto games=library.games();
        auto found=std::ranges::find_if(games,[&](const desktop::Game& value){return value.id==game->id;});
        if(found==games.end()) throw std::runtime_error("The selected game is no longer in this library.");
        found->favorite=!found->favorite;library.save_game(std::move(*found));
        host->queue_menu_reload();
      } catch(const std::exception& e) {host->emplace_overlay<app::message_overlay>(tr("Game Library"),e.what());}
    });
    return result::success;
  }
};
}

export class games_menu final : public simple_menu {
  app::shell* host_;
  std::shared_ptr<game_icon_cache> cache_;
  std::vector<std::shared_ptr<const desktop::Game>> games_;
  fs::path origin_root_;
  std::string origin_profile_;
public:
  games_menu(std::string name,auroreengine::texture&& category_icon,app::shell* host,
             auroreengine::resource_loader& loader,std::vector<desktop::Game> games,
             std::vector<std::unique_ptr<menu_entry>> utility_rows)
      :simple_menu(std::move(name),std::move(category_icon)),host_(host),cache_(std::make_shared<game_icon_cache>(loader,host)) {
    try {desktop::Library library;origin_root_=library.root();origin_profile_=library.active_profile().id;} catch(...) {}
    games_.reserve(games.size());
    for(auto& game:games) {
      auto value=std::make_shared<const desktop::Game>(std::move(game));
      games_.push_back(value);
      entries.push_back(std::make_unique<game_entry>(value,cache_,host_,origin_root_,origin_profile_));
    }
    for(auto& row:utility_rows) if(row) entries.push_back(std::move(row));
  }
  void on_open() override {simple_menu::on_open();update_selection();}
  void on_close() override {simple_menu::on_close();if(host_)host_->highlight_game(nullptr);}
  void select_submenu(unsigned int index) override {
    simple_menu::select_submenu(index);
    if(is_open)update_selection();
  }
  void prepare_icons() override {
    const auto selected=is_open&&selected_submenu<games_.size()?std::string_view(games_[selected_submenu]->id):std::string_view{};
    cache_->prepare(selected);
  }
  void get_button_actions(std::vector<std::pair<action,std::string>>& actions) override {
    if(actions.empty()&&selected_submenu<games_.size())actions.emplace_back(action::options,tr("Options"));
  }
private:
  void update_selection() {
    if(!host_) return;
    if(selected_submenu<games_.size()) {cache_->request_selected(*games_[selected_submenu]);host_->highlight_game(games_[selected_submenu]);}
    else host_->highlight_game(nullptr);
  }
};
}
