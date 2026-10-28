module;

#include "openxmb/music/player.hpp"
#include "openxmb/music/library.hpp"
#include "music_reentry.hpp"
#include "../app/input_bindings.hpp"
#include "../menu/media_preview_queue.hpp"
#include <cstring>
#include <SDL2/SDL_keyboard.h>
#include <SDL2/SDL_gamecontroller.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <functional>
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
import openxmb.config;
import :main;
import :choice_overlay;
import :message_overlay;
import :text_entry;
import :component;
import :programs;
import :xmb_presentation;

namespace programs {
using namespace app;
namespace music = openxmb::music;

export void create_music_playlist(shell* xmb, std::vector<music::QueueEntry> tracks = {}, std::function<void()> changed = {}) {
  xmb->emplace_overlay<text_entry_overlay>("New Playlist", "", [xmb, tracks=std::move(tracks), changed=std::move(changed)](std::string name) {
    try {
      const auto path = music::PlaylistStore(config::CONFIG.music_path()).create(name, tracks);
      if(changed) changed();
      xmb->emplace_overlay<message_overlay>("Playlist Created", path.stem().string() + "\n" + std::to_string(tracks.size()) + " tracks");
    } catch(const std::exception& error) { xmb->emplace_overlay<message_overlay>("Cannot Create Playlist", error.what()); }
  });
}

export void add_to_music_playlist(shell* xmb, std::vector<music::QueueEntry> tracks) {
  const auto paths = music::PlaylistStore(config::CONFIG.music_path()).list();
  std::vector<std::string> choices{"New Playlist..."};
  for(const auto& path : paths) choices.push_back(path.stem().string());
  xmb->emplace_overlay<choice_overlay>(choices, 0, [xmb, paths, tracks=std::move(tracks)](unsigned index) {
    if(index == 0) { create_music_playlist(xmb, tracks); return; }
    if(index > paths.size()) return;
    try {
      auto entries = music::read_playlist(paths[index-1], true);
      entries.insert(entries.end(), tracks.begin(), tracks.end());
      music::save_playlist_atomic(paths[index-1], entries);
      xmb->emplace_overlay<message_overlay>("Added to Playlist", std::to_string(tracks.size()) + " tracks added to " + paths[index-1].stem().string());
    } catch(const std::exception& error) { xmb->emplace_overlay<message_overlay>("Cannot Add Tracks", error.what()); }
  });
}

export class playlist_editor final : public component, public action_receiver {
public:
  float typography_scale() const override { return 2.5F; }
  playlist_editor(std::filesystem::path path, auroreengine::resource_loader&) : path_(std::move(path)) {
    try { tracks_ = music::read_playlist(path_, true); }
    catch(const std::exception& error) { notice_ = error.what(); valid_ = false; }
  }
  result tick(shell* xmb) override { xmb_ = xmb; return close_ ? result::close : result::success; }
  bool is_opaque() const override { return true; }
  bool music_background() const override { return true; }
  void render(auroreengine::gui_renderer& renderer, shell*) override {
    app::xmb_canvas c(renderer);
    c.text(path_.stem().string() + (dirty_ ? "  *" : ""), 250, 235, 38);
    c.text(std::to_string(tracks_.size()) + " tracks", 250, 284, 24);
    if(tracks_.empty()) c.text("This playlist is empty. Open Options to add tracks or a folder.", 250, 420, 28);
    const auto first = selected_ < 7 ? 0 : selected_ - 6;
    for(std::size_t i = first; i < std::min(first + 8, tracks_.size()); ++i) {
      const float y = 355 + (i-first)*62;
      if(i == selected_) c.rect(232, y-36, 1400, 55, {1,1,1,0.12F}, 5);
      c.text(std::to_string(i+1) + "  " + tracks_[i].title, 255, y, 26);
    }
    c.text("Confirm: play from track    Options: playlist    Extra: edit track    Back: close", 250, 900, 23);
    if(!notice_.empty()) c.text(notice_, 250, 965, 24);
  }
  result on_action(action value) override {
    if(!xmb_) return result::success;
    if(value == action::cancel) {
      if(!dirty_) return result::close;
      xmb_->emplace_overlay<message_overlay>("Save Playlist Changes?", path_.stem().string(), std::vector<std::string>{"Save", "Discard", "Cancel"}, [this](unsigned index) {
        if(index == 0) { if(save()) close_=true; } else if(index == 1) close_=true;
      });
    } else if(value == action::up && selected_) --selected_;
    else if(value == action::down && selected_+1 < tracks_.size()) ++selected_;
    else if(value == action::ok && !tracks_.empty()) {
      if(music::service().set_queue(tracks_, selected_)) notice_ = "Playing " + tracks_[selected_].title;
      else notice_ = music::service().snapshot().error;
    } else if(value == action::extra) track_options();
    else if(value == action::options) options();
    return result::success;
  }
private:
  void error(const std::string& text) { xmb_->emplace_overlay<message_overlay>("Playlist", text); }
  bool save() {
    if(!valid_) { error("Reload this playlist before saving; its contents could not be read."); return false; }
    try { music::save_playlist_atomic(path_, tracks_); dirty_=false; notice_="Playlist saved"; return true; }
    catch(const std::exception& failure) { error(failure.what()); return false; }
  }
  void track_options() {
    if(tracks_.empty()) { options(); return; }
    xmb_->emplace_overlay<choice_overlay>(std::vector<std::string>{"Play Now", "Play Next", "Add to Queue", "Move Up", "Move Down", "Remove from Playlist"}, 0, [this](unsigned choice) {
      if(selected_ >= tracks_.size()) return;
      if(choice == 0) { music::service().set_queue(tracks_, selected_); notice_="Playing playlist"; }
      else if(choice == 1 || choice == 2) { music::service().enqueue({tracks_[selected_]}, choice == 1); notice_=choice==1?"Track will play next":"Track added to queue"; }
      else if(choice == 3 && selected_) { std::swap(tracks_[selected_], tracks_[selected_-1]); --selected_; dirty_=true; }
      else if(choice == 4 && selected_+1 < tracks_.size()) { std::swap(tracks_[selected_], tracks_[selected_+1]); ++selected_; dirty_=true; }
      else if(choice == 5) { tracks_.erase(tracks_.begin()+selected_); if(selected_ && selected_ >= tracks_.size()) --selected_; dirty_=true; }
    });
  }
  void options() {
    xmb_->emplace_overlay<choice_overlay>(std::vector<std::string>{"Save", "Add Track / Folder / Playlist...", "Add Current Play Queue", "Rename Playlist...", "Delete Playlist...", "Reload Saved Playlist", "Edit Selected Track"}, 0, [this](unsigned choice) {
      if(choice == 0) save();
      else if(choice == 1) xmb_->emplace_overlay<text_entry_overlay>("Track, Folder or Playlist Path", config::CONFIG.music_path().string(), [this](std::string value) {
        try { auto added=music::collect_tracks(std::filesystem::path(value)); if(added.empty()) throw std::runtime_error("No supported music tracks were found"); tracks_.insert(tracks_.end(), added.begin(), added.end()); dirty_=true; }
        catch(const std::exception& failure) { error(failure.what()); }
      });
      else if(choice == 2) { const auto& added=music::service().queue().entries(); tracks_.insert(tracks_.end(),added.begin(),added.end()); dirty_=dirty_||!added.empty(); notice_=added.empty()?"The play queue is empty":"Queue added; choose Save to keep changes"; }
      else if(choice == 3) xmb_->emplace_overlay<text_entry_overlay>("Rename Playlist", path_.stem().string(), [this](std::string name) {
        try { path_=music::rename_playlist(path_,name); notice_="Playlist renamed"; }
        catch(const std::exception& failure) { error(failure.what()); }
      });
      else if(choice == 4) xmb_->emplace_overlay<message_overlay>("Delete Playlist?", "Delete " + path_.stem().string() + "? Music files are preserved.",std::vector<std::string>{"Cancel","Delete"},[this](unsigned index) {
        if(!index) return;
        try { music::delete_playlist(path_);close_=true; }
        catch(const std::exception& failure) { error(failure.what()); }
      });
      else if(choice == 5) xmb_->emplace_overlay<message_overlay>("Reload Saved Playlist?", "Discard unsaved edits and reload the saved track list?",std::vector<std::string>{"Cancel","Reload"},[this](unsigned index) {
        if(!index) return;
        try { tracks_=music::read_playlist(path_,true);selected_=0;dirty_=false;valid_=true;notice_="Saved playlist loaded"; }
        catch(const std::exception& failure) { error(failure.what()); }
      });
      else if(choice == 6) track_options();
    });
  }
  std::filesystem::path path_;
  std::vector<music::QueueEntry> tracks_;
  shell* xmb_{};
  std::array<bool,2> trigger_pressed_{};
  std::size_t selected_{};
  bool dirty_{}, close_{}, valid_{true};
  std::string notice_;
};

export class music_player final : public component, public action_receiver,
                                  public event_receiver {
public:
    float typography_scale() const override { return 2.5F; }

  music_player(std::filesystem::path path, auroreengine::resource_loader &loader)
      : loader_(loader) {
    for(const auto* name:{"volume","visual","addplaylist","delete","display","prev","next","frev","ffwd","play","pause","stop","repeat","shuffle"}) icons_.load(loader_,std::string("system/mp_")+name);
    for(const auto* hint:{"start","select","l1","r1","l2","r2","square"})icons_.load(loader_,std::string("system/btn_mp_")+hint);
    for(const auto* hint:{"start","back","lb","rb","lt","rt"})icons_.load(loader_,std::string("controller/xbox_")+hint);
    icons_.load(loader_,"theme/icon_music_default");
    for(const auto* name:{"flac","wav","mp3","aac","opus","vorbis","pcm","wma","alac","atrac"}) icons_.load(loader_,std::string("vp/fmt/")+name);
    auto &player = music::service();
    player.set_background_enabled(config::CONFIG.enabled("music.background"));
    music::reentry::open(player,path,config::CONFIG.preferences,[&] {return player.open(path);});
    queue_row_ = player.queue().current_index().value_or(0);
  }
  ~music_player() override {
    // Loading jobs retain raw texture/data pointers until their future completes.
    if (artwork_ready_.valid()) artwork_ready_.wait();
  }
  result tick(shell *xmb) override {
    xmb_ = xmb;
    std::array<bool,2> pressed{};
    for(const auto& [id,controller]:xmb->get_window()->controllers) {
      pressed[0]=pressed[0]||SDL_GameControllerGetAxis(controller.get(),SDL_CONTROLLER_AXIS_TRIGGERLEFT)>16000;
      pressed[1]=pressed[1]||SDL_GameControllerGetAxis(controller.get(),SDL_CONTROLLER_AXIS_TRIGGERRIGHT)>16000;
    }
    for(unsigned i=0;i<2;++i)if(pressed[i]&&!trigger_pressed_[i])music::service().seek(music::service().snapshot().position+(i?10.0:-10.0));
    trigger_pressed_=pressed;
    // The shell also ticks this service while the viewer is closed.
    auto &player = music::service();
    player.tick();
    queue_row_ = std::min(queue_row_, player.queue().entries().empty() ? 0 : player.queue().entries().size()-1);
    const auto &state = player.snapshot();
    const bool artwork_enabled = config::CONFIG.preference("music.artwork", "On") == "On";
    if (generation_ != state.generation || artwork_enabled_ != artwork_enabled) {
      if (artwork_ready_.valid()) artwork_ready_.wait();
      if(artwork_)xmb->get_window()->waitIdle();
      artwork_.reset();
      artwork_request_.reset();
      generation_ = state.generation;
      artwork_enabled_ = artwork_enabled;
      if (artwork_enabled && player.queue().current()) {
        artwork_request_=std::make_shared<openxmb::menu_preview::Request>();
        artwork_request_->path=player.queue().current()->path;
        artwork_request_->kind=openxmb::menu_preview::Kind::music;
      }
    }
    if(artwork_request_) {
      using openxmb::menu_preview::State;
      const auto status=artwork_request_->state.load(std::memory_order_acquire);
      if(status==State::idle)artwork_decoder_.enqueue(artwork_request_);
      if(status==State::ready&&!artwork_) {
        const auto request=artwork_request_;
        artwork_=std::make_shared<auroreengine::texture>(loader_.getDevice(),loader_.getAllocator(),request->image.width,request->image.height);
        artwork_ready_=loader_.loadTexture(artwork_.get(),auroreengine::LoaderFunction{
          [request](std::uint8_t* pixels,std::size_t capacity) {
            if(request->image.rgba.size()>capacity)throw std::runtime_error("Album art exceeds texture upload capacity");
            std::memcpy(pixels,request->image.rgba.data(),request->image.rgba.size());
          }});
      }
    }
    if(artwork_ready_.valid()&&artwork_ready_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
      try {artwork_ready_.get();}
      catch(const std::exception& error) {notice_=error.what();}
    }
    return result::success;
  }
  bool is_opaque() const override { return true; }
  bool music_background() const override { return true; }
  bool music_reactive() const override { return true; }

  void render(auroreengine::gui_renderer &renderer, shell *xmb) override {
    const auto& player=music::service();const auto& state=player.snapshot();const auto& metadata=state.metadata;
    app::xmb_canvas c(renderer);
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
    else icons_.draw(c,"theme/icon_music_default",167,941,88,0.85f);
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
    if (value == action::options && panel_ == Panel::queue) { queue_options(); return result::success; }
    if (value == action::options) { panel_ = panel_ == Panel::controls ? Panel::none : Panel::controls; return result::success; }
    if (value == action::extra) { cycle_visualizer(); return result::success; }
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
    if (value.action != action::none && (value.is<events::key_down>() || value.is<events::controller_button_down>())) return on_action(value.action);
    if (const auto *key = value.get<events::key_down>()) {
      switch (key->keycode) {
      // Shell keyboard events carry SDL scancodes, independent of layout.
      case 44: toggle_pause(); return result::success; // Space
      case 22: player.stop(); return result::success; // S
      case 17: return player.next() ? result::success : result::failure; // N
      case 19: return player.previous() ? result::success : result::failure; // P
      case 20: panel_ = Panel::queue; return result::success; // Q
      case 12: panel_ = panel_ == Panel::information ? Panel::none : Panel::information; return result::success; // I
      case 25: cycle_visualizer(); return result::success; // V
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
      if (button->button == events::logical_controller_button::back) { display_on_=!display_on_; return result::success; }
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
  void persist_transport() {
    auto& player=music::service();
    config::CONFIG.preferences["music.shuffle"]=player.queue().shuffle()?"On":"Off";
    config::CONFIG.preferences["music.repeat"]=player.queue().repeat()==music::Repeat::all?"All":player.queue().repeat()==music::Repeat::one?"One":"Off";
    config::CONFIG.save_config();
  }
  void cycle_visualizer() {
    const bool flow=config::CONFIG.preference("music.visualizer","Wave")!="Flow";
    config::CONFIG.preferences["music.visualizer"]=flow?"Flow":"Wave";
    config::CONFIG.save_config();
    music::service().set_visualizer(flow?music::Visualizer::spectrum:music::Visualizer::waves);
    notice_=flow?"Flow visualizer":"Wave visualizer";
  }
  void queue_options() {
    if(!xmb_) return;
    xmb_->emplace_overlay<choice_overlay>(std::vector<std::string>{"Save Queue as Playlist...","Add Selected to Playlist...","Move Selected Up","Move Selected Down","Remove Selected"},0,[this](unsigned choice) {
      auto& player=music::service();
      if(choice==0) { create_music_playlist(xmb_,player.queue().entries()); return; }
      if(queue_row_>=player.queue().entries().size()) return;
      if(choice==1) add_to_music_playlist(xmb_,{player.queue().entries()[queue_row_]});
      else if(choice==2&&queue_row_) {player.move(queue_row_,queue_row_-1);--queue_row_;}
      else if(choice==3&&queue_row_+1<player.queue().entries().size()) {player.move(queue_row_,queue_row_+1);++queue_row_;}
      else if(choice==4) player.remove(queue_row_);
    });
  }
  void activate_control(int) {
    auto& player=music::service();const auto& state=player.snapshot();
    const auto selected=control_row_==0?control_col_:control_row_==1?5+control_col_:12+control_col_;
    switch(selected) {
    case 0: volume_open_=!volume_open_; break;
    case 1: cycle_visualizer();break;
    case 2: panel_=Panel::queue;break;
    case 3: if(auto current=player.queue().current_index()) player.remove(*current);break;
    case 4: display_on_=!display_on_;break;
    case 5: player.previous();break;case 6:player.next();break;
    case 7:player.seek(state.position-10);break;case 8:player.seek(state.position+10);break;
    case 9:player.play();break;case 10:player.pause();break;case 11:player.stop();break;
    case 12:player.set_repeat(static_cast<music::Repeat>((static_cast<int>(player.queue().repeat())+1)%3));persist_transport();break;
    case 13:player.set_shuffle(!player.queue().shuffle());persist_transport();break;
    }
  }
  void draw_controls(auroreengine::gui_renderer& renderer) {
    app::xmb_canvas c(renderer);
    static constexpr std::array<std::string_view,14> names{"system/mp_volume","system/mp_visual","system/mp_addplaylist","system/mp_delete","system/mp_display","system/mp_prev","system/mp_next","system/mp_frev","system/mp_ffwd","system/mp_play","system/mp_pause","system/mp_stop","system/mp_repeat","system/mp_shuffle"};
    static constexpr std::array<std::string_view,14> labels{"Volume Control","Visual Player","Play Queue","Remove from Queue","Display","Previous","Next","Fast Reverse","Fast Forward","Play","Pause","Stop","Repeat","Shuffle"};
    const auto selected=control_row_==0?control_col_:control_row_==1?5+control_col_:12+control_col_;
    for(int row=0;row<3;++row) {
      const int count=row==0?5:row==1?7:2, first=row==0?0:row==1?5:12;
      const float pitch=row==2?95:64;
      for(int col=0;col<count;++col) {
        const int index=first+col;const bool focus=selected==static_cast<std::size_t>(index);
        icons_.draw(c,names[index],524+(col-(count-1)*0.5f)*pitch,412+row*65,74,focus?1:0.85f,focus?1.3f:1);
      }
    }
    const auto family=xmb_?xmb_->get_controller_type():std::string{"keyboard"};
    const bool keyboard=family=="keyboard",playstation=family=="playstation",xbox=family=="xbox";
    std::string hint_asset,hint_text;const auroreengine::texture* direct{};
    const auto use_action=[&](action value){if(xmb_){direct=xmb_->get_button_icon(value);hint_text=xmb_->get_button_label(value);}};
    const auto shortcut_available=[&](int code,bool pad){for(std::size_t i=0;i<app::input_bindings::actions.size();++i)if(app::input_bindings::read(config::CONFIG.preferences,pad,i)==code&&i!=4)return false;return true;};
    if(selected==1)use_action(action::extra);
    else if(selected==4||selected==5||selected==6||selected==9||selected==10) {
      if(keyboard) {
        const int code=selected==4?7:selected==5?19:selected==6?17:44;
        if(shortcut_available(code,false))hint_text=SDL_GetScancodeName(static_cast<SDL_Scancode>(code));else use_action(action::ok);
      } else {
        const int code=selected==4?4:selected==5?9:selected==6?10:6;
        if(!shortcut_available(code,true))use_action(action::ok);
        else if(playstation)hint_asset=std::string("system/btn_mp_")+(selected==4?"select":selected==5?"l1":selected==6?"r1":"start");
        else if(xbox)hint_asset=std::string("controller/xbox_")+(selected==4?"back":selected==5?"lb":selected==6?"rb":"start");
        else hint_text=selected==4?"View":selected==5?"LB":selected==6?"RB":"Menu";
      }
    } else if(selected==7||selected==8) {
      if(keyboard)use_action(action::ok);
      else if(playstation)hint_asset=selected==7?"system/btn_mp_l2":"system/btn_mp_r2";
      else if(xbox)hint_asset=selected==7?"controller/xbox_lt":"controller/xbox_rt";
      else hint_text=selected==7?"LT":"RT";
    }
    constexpr float size=25,height=29.16F,gap=9.6F,center=616;
    const float pixels=size*c.scale*config::CONFIG.textScale;
    const auto metrics=renderer.text_line_metrics_pixels(pixels)/c.scale;
    const float width=renderer.text_width_pixels(labels[selected],pixels)/c.scale;
    float hint_width=0;
    if(direct)hint_width=height*direct->width/std::max(1,direct->height);
    else if(!hint_asset.empty())hint_width=icons_.scaled_width(hint_asset,height);
    else if(!hint_text.empty())hint_width=std::max(height,renderer.text_width_pixels(hint_text,18*c.scale)/c.scale+14);
    const float left=524-(width+(hint_width?gap+hint_width:0))*0.5F;
    c.text(labels[selected],left,center+(metrics.x-metrics.y)*0.5F,size,{1,1,1,1},app::xmb_canvas::pulse());
    const auto hint_x=left+width+gap;
    if(direct)c.image(*direct,hint_x,center-height*0.5F,hint_width,height);
    else if(!hint_asset.empty())icons_.draw(c,hint_asset,hint_x+hint_width*0.5F,center,height,0.95F);
    else if(!hint_text.empty()) {c.rect(hint_x,center-height*0.5F,hint_width,height,{0.8F,0.8F,0.8F,0.9F},4);c.rect(hint_x+1,center-height*0.5F+1,hint_width-2,height-2,{0.06F,0.06F,0.06F,1},3);c.text(hint_text,hint_x+hint_width*0.5F,center+6,18,{1,1,1,1},0,true);}
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
    renderer.draw_text("Options: save / add to playlist / reorder / remove", 0.42f, 0.73f, 0.020f);
  }
  auroreengine::resource_loader &loader_;
  Panel panel_{Panel::none};
  app::media_icons icons_;
  bool display_on_{true},volume_open_{false},artwork_enabled_{true};
  shell* xmb_{};
  std::array<bool,2> trigger_pressed_{};
  std::size_t control_row_{1},control_col_{4},queue_row_{};
  std::uint64_t generation_{~std::uint64_t{}};
  std::shared_ptr<auroreengine::texture> artwork_;
  openxmb::menu_preview::Queue artwork_decoder_;
  std::shared_ptr<openxmb::menu_preview::Request> artwork_request_;
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
