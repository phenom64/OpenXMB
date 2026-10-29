module;
#include "openxmb/desktop/async_job.hpp"
#include "openxmb/desktop/youtube.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

export module openxmb.app:youtube_setup;
import :component;
import :main;
import :xmb_presentation;
import :dialog_chrome;
import :message_overlay;
import auroreengine;
import glm;
import openxmb.utils;

namespace app {
export class youtube_setup final : public component, public action_receiver {
  struct Transfer {
    std::mutex mutex;
    std::string stage{"Preparing YouTube"};
    std::uint64_t downloaded{},total{};
  };
  std::shared_ptr<Transfer> transfer_=std::make_shared<Transfer>();
  openxmb::desktop::AsyncJob<openxmb::desktop::YoutubeInstallResult> job_;
  std::string stage_{"Preparing YouTube"};
  std::uint64_t downloaded_{},total_{};
  bool cancelled_{};
  static std::string friendly(std::string text) {
    for(std::size_t at=0;(at=text.find("VacuumTube",at))!=std::string::npos;at+=7)text.replace(at,10,"YouTube");
    return text;
  }
public:
  explicit youtube_setup(std::filesystem::path root) {
    const auto transfer=transfer_;
    job_.start([root=std::move(root),transfer](std::stop_token stop) {
      using namespace openxmb::desktop;
      const HttpTransport transport=[stop](HttpRequest request) {
        request.progress=[stop,previous=request.progress](std::uint64_t done,std::uint64_t total) {
          return !stop.stop_requested() && (!previous || previous(done,total));
        };
        return http_get(request);
      };
      const auto release=fetch_youtube_release(youtube_platform(),youtube_arch(),transport);
      if(!release)return YoutubeInstallResult{{},release.error};
      if(stop.stop_requested())return YoutubeInstallResult{{},"Setup was canceled."};
      return install_youtube(root,*release.value,transport,[transfer,stop](std::string_view stage,std::uint64_t done,std::uint64_t total) {
        if(stop.stop_requested())return false;
        std::lock_guard lock(transfer->mutex);transfer->stage=friendly(std::string(stage));transfer->downloaded=done;transfer->total=total;return true;
      });
    });
  }
  bool is_opaque() const override {return true;}
  bool do_fade_in() const override {return true;}
  bool do_fade_out() const override {return true;}
  float fade_zoom() const override {return 1.0F;}
  result tick(shell* host) override {
    if(cancelled_)return result::close;
    {std::lock_guard lock(transfer_->mutex);stage_=transfer_->stage;downloaded_=transfer_->downloaded;total_=transfer_->total;}
    if(job_.ready()) {
      try {
        const auto completed=job_.get();
        if(completed) {host->queue_menu_reload();host->queue_panel("youtube");}
        else host->emplace_overlay<message_overlay>("YouTube Setup",friendly(completed.error),std::vector<std::string>{"OK"});
      } catch(...) {host->emplace_overlay<message_overlay>("YouTube Setup","Setup could not finish. Your existing application and files have been preserved.",std::vector<std::string>{"OK"});}
      return result::close;
    }
    return result::success;
  }
  result on_action(action input) override {
    if(input==action::cancel) {job_.cancel();cancelled_=true;return result::success;}
    return result::success;
  }
  void render(auroreengine::gui_renderer& renderer,shell* host) override {
    const xmb_canvas c(renderer);
    c.rect(0,0,1920,1080,{0,0,0,0.2F});dialog_chrome::divider_frame(c,1);
    c.text("YouTube",180,171,35);
    c.text("Getting YouTube ready",960,420,32,{1,1,1,1},0,true);
    c.text(stage_,960,485,25,{1,1,1,0.85F},0,true);
    c.rect(560,555,800,10,{0.15F,0.15F,0.15F,1},4);
    if(total_) {
      const auto amount=std::clamp(static_cast<float>(downloaded_)/static_cast<float>(total_),0.0F,1.0F);
      if(amount>0)c.rect(560,555,800*amount,10,{0.94F,0.94F,0.94F,1},4);
      const auto text=std::to_string(downloaded_/1048576)+" / "+std::to_string((total_+1048575)/1048576)+" MB";
      c.text(text,960,613,22,{1,1,1,0.72F},0,true);
    } else {
      const auto now=std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
      const auto position=0.5F+0.5F*std::sin(now*2.0F);
      c.rect(560+position*640,555,160,10,{0.9F,0.9F,0.9F,1},4);
    }
    c.text("You can stay here while setup completes.",960,700,24,{1,1,1,0.78F},0,true);
    host->render_message_footer(renderer,std::array{std::pair{action::cancel,"Cancel"}},1);
  }
};
} // namespace app
