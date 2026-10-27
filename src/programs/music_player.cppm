module;

#include "openxmb/music/player.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <future>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>

export module openxmb.app:music_player;

import auroreengine;
import glm;
import openxmb.utils;
import :component;
import :programs;
import :xmb_presentation;

namespace programs {
using namespace app;
namespace music = openxmb::music;

export class music_player final : public component, public action_receiver,
                                  public event_receiver {
public:
    float typography_scale() const override { return 2.5F; }

  music_player(std::filesystem::path path, auroreengine::resource_loader &loader)
      : loader_(loader) {
    for(const auto* name:{"volume","visual","addplaylist","delete","display","prev","next","frev","ffwd","play","pause","stop","repeat","shuffle"}) icons_.load(loader_,std::string("system/mp_")+name);
    for(const auto* name:{"flac","wav","mp3","aac","opus","vorbis","pcm","wma","alac","atrac"}) icons_.load(loader_,std::string("vp/fmt/")+name);
    for(const auto* name:{"l1","r1","l2","r2","start","select","square"}) icons_.load(loader_,std::string("system/btn_mp_")+name);
    auto &player = music::service();
    const auto *current = player.queue().current();
    std::error_code error;
    if (!current || !std::filesystem::equivalent(current->path, path, error))
      player.open(path);
    queue_row_ = player.queue().current_index().value_or(0);
  }
  ~music_player() override {
    // Loading jobs retain raw texture/data pointers until their future completes.
    if (artwork_ready_.valid()) artwork_ready_.wait();
  }
  result tick(shell *) override {
    // The shell also ticks this service while the viewer is closed.
    auto &player = music::service();
    player.tick();
    const auto &state = player.snapshot();
    if (generation_ != state.generation) {
      if (artwork_ready_.valid()) artwork_ready_.wait();
      artwork_.reset();
      artwork_data_ = state.metadata.artwork;
      generation_ = state.generation;
      if (artwork_data_ || !state.metadata.artwork_path.empty()) {
        artwork_ = std::make_shared<auroreengine::texture>(loader_.getDevice(), loader_.getAllocator());
        if (artwork_data_) artwork_ready_ = loader_.loadTexture(artwork_.get(), auroreengine::LoadDataView(*artwork_data_, state.metadata.artwork_type));
        else artwork_ready_ = loader_.loadTexture(artwork_.get(), state.metadata.artwork_path);
      }
    }
    return result::success;
  }
  bool is_opaque() const override { return true; }
  bool music_background() const override { return true; }

  void render(auroreengine::gui_renderer &renderer, shell *) override {
    const auto& player=music::service();const auto& state=player.snapshot();const auto& metadata=state.metadata;
    app::xmb_canvas c(renderer);
    if(visualizer_on_) draw_visualizer(renderer,state);
    if(panel_==Panel::controls) draw_controls(renderer);
    else if(panel_==Panel::queue) draw_queue(renderer);
    else if(panel_==Panel::information) {
      c.rect(1090,230,660,500,{0,0,0,0.7f});
      const std::array lines{"Album: "+metadata.album,"Artist: "+metadata.artist,"Genre: "+metadata.genre,
        "Date: "+metadata.date,"Track: "+metadata.track,"Codec: "+metadata.codec+"  "+std::to_string(metadata.sample_rate)+" Hz",
        "Channels: "+std::to_string(metadata.channels)+"  "+std::to_string(metadata.bit_rate/1000)+" kb/s"};
      float y=285;for(const auto& line:lines) {c.text(elide(line,52),1130,y,23);y+=48;}
    }
    if(display_on_) {
      icons_.draw(c,state.status==music::Status::playing?"system/mp_play":"system/mp_pause",154,846,49,1);
      if(player.queue().repeat()!=music::Repeat::off) icons_.draw(c,"system/mp_repeat",270,846,49,1);
      if(player.queue().shuffle()) icons_.draw(c,"system/mp_shuffle",375,846,49,1);
      const auto index=player.queue().current_index();
      c.text(std::to_string(index?*index+1:0)+" / "+std::to_string(player.queue().entries().size()),1740,858,25,{1,1,1,0.95f});
      c.text(time_text(state.position)+" / "+time_text(metadata.duration),1418,942,27);
      std::string format=metadata.codec;
      if(const auto* track=player.queue().current()) {
        format=track->path.extension().string();if(!format.empty()&&format.front()=='.') format.erase(0,1);
        std::transform(format.begin(),format.end(),format.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
      }
      if(metadata.codec=="flac"||metadata.codec=="mp3"||metadata.codec=="opus"||metadata.codec=="alac"||metadata.codec=="vorbis") format=metadata.codec;
      if(format=="m4a"||format=="mp4") format="aac";else if(format=="aiff"||format=="aif") format="pcm";else if(format=="ogg"||format=="oga") format="vorbis";
      icons_.draw(c,"vp/fmt/"+format,1842,929,27,1);
      const float progress=metadata.duration>0?std::clamp(static_cast<float>(state.position/metadata.duration),0.0f,1.0f):0;
      c.rect(1418,967,385,11,{0.48f,0.48f,0.48f,1},5.5f);c.rect(1419,968,383,8,{0.045f,0.045f,0.045f,1},4);
      c.rect(1419,968,383*progress,8,{0.30f,0.49f,0.008f,1},std::min(4.0F,191.5F*progress));c.rect(1419,968,383*progress,2,{0.78f,0.92f,0.35f,1});
    }
    if(artwork_&&artwork_->loaded) c.image(*artwork_,123,897,88,88);
    else icons_.draw(c,"system/mp_visual",167,941,88,0.85f);
    c.text(elide(metadata.title.empty()?"Music":metadata.title,58),243,941,34);
    c.text(elide(metadata.artist+(metadata.album.empty()?"":"  /  "+metadata.album),90),243,981,25);
    if(!state.error.empty()) c.text(elide(state.error,100),123,785,24,{1,0.7f,0.5f,1});
    else if(!notice_.empty()) c.text(elide(notice_,100),123,785,24);
    if(volume_open_) c.text("Volume  "+std::to_string(static_cast<int>(state.volume*100))+"%",524,682,25,{1,1,1,1},0,true);
  }
  result on_action(action value) override {
    auto &player = music::service();
    const auto &state = player.snapshot();
    if (value == action::cancel) {
      if (panel_ != Panel::none) { panel_ = Panel::none; return result::success; }
      if (!state.background) player.stop();
      return result::close;
    }
    if (value == action::options) { panel_ = panel_ == Panel::controls ? Panel::none : Panel::controls; return result::success; }
    if (value == action::extra) { visualizer_on_=!visualizer_on_; cycle_visualizer(); return result::success; }
    if (panel_ == Panel::queue) {
      const auto count = player.queue().entries().size();
      if (count) queue_row_ = std::min(queue_row_, count - 1);
      if (value == action::up && queue_row_) --queue_row_;
      else if (value == action::down && queue_row_ + 1 < count) ++queue_row_;
      else if (value == action::ok) return player.select(queue_row_) ? result::success : result::failure;
      else return result::unsupported;
      return result::success;
    }
    if(volume_open_&&(value==action::left||value==action::right)) {player.set_volume(state.volume+(value==action::left?-0.05F:0.05F));return result::success;}
    if(panel_==Panel::controls) {
      static constexpr std::array<int,3> counts{5,7,2};
      if(value==action::left) control_col_=(control_col_+counts[control_row_]-1)%counts[control_row_];
      else if(value==action::right) control_col_=(control_col_+1)%counts[control_row_];
      else if(value==action::up||value==action::down) {control_row_=(control_row_+(value==action::up?2:1))%3;control_col_=std::min(control_col_,static_cast<std::size_t>(counts[control_row_]-1));}
      else if(value==action::ok) activate_control(1);
      else return result::unsupported;
      return result::success;
    }
    switch (value) {
    case action::ok: toggle_pause(); return result::success;
    case action::left: return player.seek(state.position - 10.0) ? result::success : result::failure;
    case action::right: return player.seek(state.position + 10.0) ? result::success : result::failure;
    case action::up: player.set_volume(state.volume + 0.05f); return result::success;
    case action::down: player.set_volume(state.volume - 0.05f); return result::success;
    default: return result::unsupported;
    }
  }
  result on_event(const event &value) override {
    auto &player = music::service();
    if (const auto *key = value.get<events::key_down>()) {
      switch (key->keycode) {
      // Shell keyboard events carry SDL scancodes, independent of layout.
      case 44: toggle_pause(); return result::success; // Space
      case 22: player.stop(); return result::success; // S
      case 17: return player.next() ? result::success : result::failure; // N
      case 19: return player.previous() ? result::success : result::failure; // P
      case 20: panel_ = Panel::queue; return result::success; // Q
      case 12: panel_ = panel_ == Panel::information ? Panel::none : Panel::information; return result::success; // I
      case 25: visualizer_on_=!visualizer_on_; cycle_visualizer(); return result::success; // V
      case 7: display_on_=!display_on_; return result::success; // D
      case 16: player.set_muted(!player.snapshot().muted); return result::success; // M
      case 46: case 87: player.set_volume(player.snapshot().volume + 0.05f); return result::success;
      case 45: case 86: player.set_volume(player.snapshot().volume - 0.05f); return result::success;
      case 76: // Delete removes the selected queued track.
        if (panel_ == Panel::queue) return player.remove(queue_row_) ? result::success : result::failure;
        break;
      default: break;
      }
    }
    if (const auto *button = value.get<events::controller_button_down>()) {
      if (button->button == events::logical_controller_button::leftshoulder) return player.previous() ? result::success : result::failure;
      if (button->button == events::logical_controller_button::rightshoulder) return player.next() ? result::success : result::failure;
      if (button->button == events::logical_controller_button::start) { toggle_pause(); return result::success; }
    }
    if (const auto *scroll = value.get<events::mouse_scroll>()) {
      player.set_volume(player.snapshot().volume + scroll->x * 0.05f);
      return result::success;
    }
    return on_action(value.action);
  }

private:
  enum class Panel { none, controls, queue, information };
  constexpr static std::size_t control_count = 14;
  static std::string elide(const std::string &text, std::size_t count) {
    // Keep UTF-8 code points intact when truncating long file/tag strings.
    if (text.size() <= count) return text;
    while (count && (static_cast<unsigned char>(text[count]) & 0xc0) == 0x80) --count;
    return text.substr(0, count) + "...";
  }
  static std::string time_text(double seconds) {
    const auto whole = std::isfinite(seconds) && seconds > 0.0 ? static_cast<unsigned long long>(seconds) : 0;
    const auto two=[](unsigned long long value){return (value<10?"0":"")+std::to_string(value);};
    return two(whole/3600)+":"+two(whole/60%60)+":"+two(whole%60);
  }
  static std::string repeat_name(music::Repeat repeat) {
    switch (repeat) { case music::Repeat::all: return "Repeat all"; case music::Repeat::one: return "Repeat one"; default: return "Repeat off"; }
  }
  static std::string status_name(music::Status status) {
    switch (status) { case music::Status::playing: return "Playing"; case music::Status::paused: return "Paused"; case music::Status::ended: return "End of queue"; case music::Status::error: return "Playback error"; default: return "Stopped"; }
  }
  void toggle_pause() {
    auto &player = music::service();
    if (player.snapshot().status == music::Status::playing) player.pause();
    else player.play();
  }
  void cycle_visualizer() {
    auto &player = music::service();
    player.set_visualizer(static_cast<music::Visualizer>((static_cast<int>(player.snapshot().visualizer) + 1) % 3));
  }
  void activate_control(int) {
    auto& player=music::service();const auto& state=player.snapshot();
    const auto selected=control_row_==0?control_col_:control_row_==1?5+control_col_:12+control_col_;
    switch(selected) {
    case 0: volume_open_=!volume_open_; break;
    case 1: visualizer_on_=!visualizer_on_;cycle_visualizer();break;
    case 2: panel_=Panel::queue;break;
    case 3: if(auto current=player.queue().current_index()) player.remove(*current);break;
    case 4: display_on_=!display_on_;break;
    case 5: player.previous();break;case 6:player.next();break;
    case 7:player.seek(state.position-10);break;case 8:player.seek(state.position+10);break;
    case 9:player.play();break;case 10:player.pause();break;case 11:player.stop();break;
    case 12:player.set_repeat(static_cast<music::Repeat>((static_cast<int>(player.queue().repeat())+1)%3));break;
    case 13:player.set_shuffle(!player.queue().shuffle());break;
    }
  }
  void save_playlist() {
    auto &player = music::service();
    const auto *current = player.queue().current();
    if (!current) return;
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto destination = current->path.parent_path() / ("OpenXMB Playlist " + std::to_string(stamp) + ".m3u8");
    if (player.save_playlist(destination)) notice_ = "Saved " + destination.filename().string();
  }
  void draw_controls(auroreengine::gui_renderer& renderer) {
    app::xmb_canvas c(renderer);
    static constexpr std::array<std::string_view,14> names{"volume","visual","addplaylist","delete","display","prev","next","frev","ffwd","play","pause","stop","repeat","shuffle"};
    static constexpr std::array<std::string_view,14> labels{"Volume Control","Visual Player","Play Queue","Remove from Queue","Display","Previous","Next","Fast Reverse","Fast Forward","Play","Pause","Stop","Repeat","Shuffle"};
    const auto selected=control_row_==0?control_col_:control_row_==1?5+control_col_:12+control_col_;
    for(int row=0;row<3;++row) {
      const int count=row==0?5:row==1?7:2, first=row==0?0:row==1?5:12;
      const float pitch=row==2?95:64;
      for(int col=0;col<count;++col) {
        const int index=first+col;const bool focus=selected==static_cast<std::size_t>(index);
        icons_.draw(c,std::string("system/mp_")+std::string(names[index]),524+(col-(count-1)*0.5f)*pitch,412+row*65,74,focus?1:0.85f,focus?1.3f:1);
      }
    }
    static constexpr std::array<std::string_view,14> hints{"","select","","","square","l1","r1","l2","r2","start","start","","",""};
    const float label_width=renderer.text_width_pixels(std::string(labels[selected]),25*c.scale);
    const bool has_hint=!hints[selected].empty();
    c.text(labels[selected],has_hint?524-31:524,632,25,{1,1,1,1},app::xmb_canvas::pulse(),true);
    if(has_hint) icons_.draw(c,"system/btn_mp_"+std::string(hints[selected]),524+label_width/(2*c.scale)+18,616,28,0.9f);
  }
  void draw_queue(auroreengine::gui_renderer &renderer) {
    const auto &queue = music::service().queue();
    const auto &entries = queue.entries();
    const auto first = queue_row_ < 6 ? 0 : queue_row_ - 5;
    for (std::size_t row = first; row < std::min(first + 7, entries.size()); ++row) {
      const float y = 0.28f + static_cast<float>(row - first) * 0.060f;
      if (row == queue_row_) renderer.draw_rect({0.40f, y - 0.008f}, {0.52f, 0.050f}, {1.0f, 1.0f, 1.0f, 0.14f});
      const auto prefix = queue.current_index() == row ? "> " : "  ";
      renderer.draw_text(prefix + elide(entries[row].title, 46), 0.42f, y, 0.026f);
    }
    renderer.draw_text("Delete: remove selected track", 0.42f, 0.73f, 0.020f);
  }
  void draw_visualizer(auroreengine::gui_renderer &renderer, const music::Snapshot &state) {
    const auto elapsed = static_cast<float>(std::fmod(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(), 1000.0));
    for (std::size_t index = 0; index < state.spectrum.size(); ++index) {
      const float amplitude = state.spectrum[index];
      const float fraction = static_cast<float>(index) / state.spectrum.size();
      if (state.visualizer == music::Visualizer::spectrum) {
        renderer.draw_rect({0.40f + fraction * 0.52f, 0.72f - amplitude * 0.25f}, {0.015f, 0.006f + amplitude * 0.25f}, {0.40f, 0.70f, 1.0f, 0.42f});
      } else if (state.visualizer == music::Visualizer::globe) {
        const float angle = fraction * 2.0f * std::numbers::pi_v<float> + elapsed * 0.1f;
        const float radius = 0.12f + amplitude * 0.10f;
        renderer.draw_rect({0.66f + std::cos(angle) * radius, 0.50f + std::sin(angle) * radius}, {0.009f + amplitude * 0.01f, 0.012f}, {0.50f, 0.80f, 1.0f, 0.45f});
      } else {
        const float y = 0.52f + std::sin(fraction * 9.0f + elapsed * 0.3f) * (0.05f + amplitude * 0.15f);
        renderer.draw_rect({0.35f + fraction * 0.62f, y}, {0.028f, 0.004f + amplitude * 0.018f}, {0.45f, 0.72f, 1.0f, 0.4f});
      }
    }
  }
  auroreengine::resource_loader &loader_;
  Panel panel_{Panel::none};
  app::media_icons icons_;
  bool display_on_{true},visualizer_on_{false},volume_open_{false};
  std::size_t control_row_{1},control_col_{4},queue_row_{};
  std::uint64_t generation_{~std::uint64_t{}};
  std::shared_ptr<auroreengine::texture> artwork_;
  std::shared_ptr<const std::vector<std::uint8_t>> artwork_data_;
  std::future<void> artwork_ready_;
  std::string notice_;
};

namespace {
const inline register_program<music_player> music_player_program{
    "Music Player",
    {"audio/mpeg", "audio/wav", "audio/x-wav", "audio/ogg", "audio/flac", "audio/x-flac", "audio/mp4", "audio/aac", "audio/x-ms-wma", "audio/x-mpegurl", "application/vnd.apple.mpegurl", "audio/x-scpls"},
    {".mp3", ".wav", ".ogg", ".oga", ".opus", ".flac", ".m4a", ".aac", ".wma", ".aif", ".aiff", ".mod", ".xm", ".it", ".s3m", ".m3u", ".m3u8", ".pls"}};
}
} // namespace programs
