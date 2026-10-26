module;

#include "openxmb/music/player.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>

export module openxmb.app:music_player;

import dreamrender;
import glm;
import openxmb.utils;
import :component;
import :programs;

namespace programs {
using namespace app;
namespace music = openxmb::music;

export class music_player final : public component, public action_receiver,
                                  public event_receiver {
public:
    float typography_scale() const override { return 2.5F; }

  music_player(std::filesystem::path path, dreamrender::resource_loader &loader)
      : loader_(loader) {
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
        artwork_ = std::make_shared<dreamrender::texture>(loader_.getDevice(), loader_.getAllocator());
        if (artwork_data_) artwork_ready_ = loader_.loadTexture(artwork_.get(), dreamrender::LoadDataView(*artwork_data_, state.metadata.artwork_type));
        else artwork_ready_ = loader_.loadTexture(artwork_.get(), state.metadata.artwork_path);
      }
    }
    return result::success;
  }
  bool is_opaque() const override { return true; }

  void render(dreamrender::gui_renderer &renderer, shell *xmb) override {
    const auto &player = music::service();
    const auto &state = player.snapshot();
    const auto &metadata = state.metadata;
    const auto foreground = glm::vec4(1.0f, 1.0f, 1.0f, 0.96f);
    renderer.draw_rect({0.0f, 0.0f}, {1.0f, 1.0f}, {0.015f, 0.025f, 0.06f, 1.0f});
    draw_visualizer(renderer, state);
    renderer.draw_text(elide(metadata.title.empty() ? "Music" : metadata.title, 62), 0.08f, 0.10f, 0.055f, foreground);
    renderer.draw_text(elide(metadata.artist + (metadata.album.empty() ? "" : "  /  " + metadata.album), 86), 0.08f, 0.17f, 0.030f, foreground);
    if (artwork_ && artwork_->loaded) renderer.draw_image(artwork_->imageView.get(), 0.08f, 0.27f, 0.28f * renderer.aspect_ratio, 0.28f);
    else {
      renderer.draw_rect({0.08f, 0.27f}, {0.22f, 0.28f}, {0.15f, 0.20f, 0.29f, 0.7f});
      renderer.draw_text("Music", 0.19f, 0.41f, 0.035f, foreground, true, true);
    }
    renderer.draw_text(status_name(state.status), 0.08f, 0.61f, 0.029f, foreground);
    const auto current = player.queue().current_index();
    renderer.draw_text(std::to_string(current ? *current + 1 : 0) + " / " + std::to_string(player.queue().entries().size()) +
                           (player.queue().shuffle() ? "   Shuffle" : "") +
                           "   " + repeat_name(player.queue().repeat()), 0.08f, 0.66f, 0.025f, foreground);
    renderer.draw_text("Volume " + std::to_string(static_cast<int>(state.volume * 100.0f)) + "%" +
                           (state.muted ? "  Muted" : state.ducked ? "  Ducked" : "") +
                           (state.background ? "  Background on" : ""), 0.08f, 0.71f, 0.023f, foreground);

    if (panel_ == Panel::controls) draw_controls(renderer);
    else if (panel_ == Panel::queue) draw_queue(renderer);
    else if (panel_ == Panel::information) {
      const std::array lines{
          "Album: " + metadata.album, "Artist: " + metadata.artist,
          "Genre: " + metadata.genre, "Date: " + metadata.date, "Track: " + metadata.track,
          "Codec: " + metadata.codec + "  " + std::to_string(metadata.sample_rate) + " Hz",
          "Channels: " + std::to_string(metadata.channels) + "  " + std::to_string(metadata.bit_rate / 1000) + " kb/s"};
      float y = 0.29f;
      for (const auto &line : lines) { renderer.draw_text(elide(line, 64), 0.43f, y, 0.025f, foreground); y += 0.055f; }
    }
    const float progress = metadata.duration > 0.0 ? static_cast<float>(std::clamp(state.position / metadata.duration, 0.0, 1.0)) : 0.0f;
    renderer.draw_rect({0.08f, 0.83f}, {0.84f, 0.007f}, {1.0f, 1.0f, 1.0f, 0.20f});
    renderer.draw_rect({0.08f, 0.83f}, {0.84f * progress, 0.007f}, {0.45f, 0.74f, 1.0f, 1.0f});
    renderer.draw_text(time_text(state.position) + " / " + time_text(metadata.duration), 0.08f, 0.86f, 0.023f, foreground);
    if (!state.error.empty()) renderer.draw_text(elide(state.error, 100), 0.08f, 0.77f, 0.023f, {1.0f, 0.65f, 0.45f, 1.0f});
    else if (!notice_.empty()) renderer.draw_text(elide(notice_, 100), 0.08f, 0.77f, 0.023f, foreground);
    render_controller_buttons(xmb, renderer, 0.5f, 0.94f, std::array{
        std::pair{action::ok, std::string_view{panel_ == Panel::queue ? "Play track" : "Play / Pause"}},
        std::pair{action::options, std::string_view{"Controls"}},
        std::pair{action::extra, std::string_view{"Visualizer"}},
        std::pair{action::cancel, std::string_view{panel_ == Panel::none ? "Return to XMB" : "Back"}}});
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
    if (panel_ == Panel::controls) {
      if (value == action::up) control_row_ = (control_row_ + control_count - 1) % control_count;
      else if (value == action::down) control_row_ = (control_row_ + 1) % control_count;
      else if (value == action::ok || value == action::right || value == action::left) activate_control(value == action::left ? -1 : 1);
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
      case 25: cycle_visualizer(); return result::success; // V
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
    return std::to_string(whole / 60) + ':' + (whole % 60 < 10 ? "0" : "") + std::to_string(whole % 60);
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
  void activate_control(int direction) {
    auto &player = music::service();
    const auto &state = player.snapshot();
    switch (control_row_) {
    case 0: toggle_pause(); break;
    case 1: player.previous(); break;
    case 2: player.next(); break;
    case 3: player.stop(); break;
    case 4: player.set_shuffle(!player.queue().shuffle()); break;
    case 5: player.set_repeat(static_cast<music::Repeat>((static_cast<int>(player.queue().repeat()) + (direction > 0 ? 1 : 2)) % 3)); break;
    case 6: cycle_visualizer(); break;
    case 7: player.set_volume(state.volume + direction * 0.05f); break;
    case 8: panel_ = Panel::queue; break;
    case 9: save_playlist(); break;
    case 10: player.set_background_enabled(!state.background); break;
    case 11: player.set_ducking_enabled(!state.ducking); break;
    case 12: player.set_muted(!state.muted); break;
    case 13: panel_ = Panel::information; break;
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
  void draw_controls(dreamrender::gui_renderer &renderer) {
    const auto &player = music::service();
    const auto &state = player.snapshot();
    const std::array<std::string, control_count> labels{
        state.status == music::Status::playing ? "Pause" : "Play", "Previous track", "Next track", "Stop",
        std::string("Shuffle: ") + (player.queue().shuffle() ? "On" : "Off"), repeat_name(player.queue().repeat()),
        "Change visualizer", "Volume: " + std::to_string(static_cast<int>(state.volume * 100)) + "%", "Play queue",
        "Save playlist", std::string("Background: ") + (state.background ? "On" : "Off"),
        std::string("Duck other audio: ") + (state.ducking ? "On" : "Off"), state.muted ? "Unmute" : "Mute", "Information"};
    const auto first = control_row_ < 7 ? 0 : control_row_ - 6;
    for (std::size_t row = first; row < std::min(first + 7, labels.size()); ++row) {
      const float y = 0.29f + static_cast<float>(row - first) * 0.060f;
      if (row == control_row_) renderer.draw_rect({0.41f, y - 0.009f}, {0.48f, 0.050f}, {1.0f, 1.0f, 1.0f, 0.14f});
      renderer.draw_text(labels[row], 0.43f, y, 0.027f);
    }
  }
  void draw_queue(dreamrender::gui_renderer &renderer) {
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
  void draw_visualizer(dreamrender::gui_renderer &renderer, const music::Snapshot &state) {
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
  dreamrender::resource_loader &loader_;
  Panel panel_{Panel::none};
  std::size_t control_row_{}, queue_row_{};
  std::uint64_t generation_{~std::uint64_t{}};
  std::shared_ptr<dreamrender::texture> artwork_;
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
