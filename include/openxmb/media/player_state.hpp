#pragma once

#include "openxmb/media/catalog.hpp"
#include "openxmb/media/clock.hpp"
#include "openxmb/media/error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace openxmb::media {

enum class MediaKind { photo, music, video };

enum class PlayerStatus {
  unloaded,
  stopped,
  playing,
  paused,
  scanning,
  buffering,
  ended,
  error,
};

enum class RepeatMode { off, all, one, title, a_b, folder };
enum class ScanDirection { reverse = -1, forward = 1 };

enum class ScreenMode {
  normal,
  full_screen,
  original,
  zoom,
  double_scale,
};

enum class PlayerPanel {
  none,
  control_panel,
  display,
  scene_search,
  go_to,
  audio_options,
  subtitle_options,
  volume_control,
  av_settings,
  playlist,
  information,
  trimming,
  wallpaper,
};

enum class FocusTarget {
  media,
  panel,
  submenu,
  timeline,
  volume,
  scene_grid,
  dialog,
};

enum class PlayerSubmenu {
  none,
  screen_mode,
  repeat,
  audio,
  subtitle,
  volume,
  av_settings,
  visualizer,
  playlist,
};

enum class MusicVisualizer { xmb_waves, canyon, globe };

enum class Enhancement {
  block_noise_reduction,
  frame_noise_reduction,
  mosquito_noise_reduction,
  upscale,
};

struct EnhancementSettings {
  bool block_noise_reduction{false};
  bool frame_noise_reduction{false};
  bool mosquito_noise_reduction{false};
  bool upscale{false};
};

struct ZoomPanState {
  double zoom{1.0};
  double pan_x{};
  double pan_y{};
};

struct RepeatSection {
  std::optional<MediaTime> point_a;
  std::optional<MediaTime> point_b;
};

struct BufferingState {
  bool active{false};
  std::optional<double> progress;
  std::string reason;
};

enum class IssueSeverity { degraded, fatal };

struct PlayerIssue {
  IssueSeverity severity{IssueSeverity::degraded};
  MediaError error;
  bool recoverable{true};
};

struct PlayerState {
  std::optional<MediaKind> media_kind;
  MediaId item_id;
  std::optional<MediaTime> duration;
  MediaTime position{};
  PlayerStatus status{PlayerStatus::unloaded};
  double playback_rate{1.0};
  RepeatMode repeat{RepeatMode::off};
  RepeatSection repeat_section;
  bool shuffle{false};
  std::int8_t volume_level{}; // Firmware-compatible -4 through +4.
  bool muted{false};
  bool audio_present{false};
  ScreenMode screen_mode{ScreenMode::normal};
  ZoomPanState zoom_pan;
  PlayerPanel panel{PlayerPanel::none};
  FocusTarget focus{FocusTarget::media};
  PlayerSubmenu submenu{PlayerSubmenu::none};
  bool osd_visible{false};
  bool full_information_visible{false};
  MusicVisualizer visualizer{MusicVisualizer::xmb_waves};
  BufferingState buffering;
  std::optional<PlayerIssue> issue;
  std::vector<MediaId> available_subtitle_ids;
  std::vector<MediaId> available_audio_ids;
  std::optional<MediaId> selected_subtitle_id; // Empty means Off.
  std::optional<MediaId> selected_audio_id;
  AudioChannelMode audio_channel{AudioChannelMode::stereo};
  std::vector<CodecBadge> codec_badges;
  EnhancementSettings enhancements;
};

enum class PlayerEffect {
  none,
  decoder_load,
  decoder_unload,
  decoder_play,
  decoder_pause,
  decoder_stop,
  decoder_seek,
  decoder_set_rate,
  decoder_scan,
  decoder_select_subtitle,
  decoder_select_audio,
  decoder_set_volume,
  renderer_reconfigure,
  advance_item,
  ui_only,
  recover_backend,
};

struct PlayerTransition {
  PlayerStatus previous_status{PlayerStatus::unloaded};
  PlayerStatus current_status{PlayerStatus::unloaded};
  MediaTime previous_position{};
  MediaTime current_position{};
  PlayerEffect effect{PlayerEffect::none};
  std::string detail;
};

using PlayerResult = MediaResult<PlayerTransition>;

struct PlayerLoadRequest {
  MediaKind media_kind{MediaKind::music};
  MediaId item_id;
  std::optional<MediaTime> duration;
  MediaTime initial_position{};
  bool audio_present{false};
  bool start_playing{true};
  std::vector<MediaId> subtitle_ids;
  std::vector<MediaId> audio_ids;
  std::optional<MediaId> default_subtitle_id;
  std::optional<MediaId> default_audio_id;
  std::vector<CodecBadge> codec_badges;
};

class PlayerStateMachine {
public:
  [[nodiscard]] const PlayerState &state() const noexcept { return state_; }

  [[nodiscard]] PlayerResult load(PlayerLoadRequest request);
  [[nodiscard]] PlayerResult unload();
  [[nodiscard]] PlayerResult play();
  [[nodiscard]] PlayerResult pause();
  [[nodiscard]] PlayerResult stop();
  [[nodiscard]] PlayerResult seek(MediaTime position);
  [[nodiscard]] PlayerResult set_rate(double rate);
  [[nodiscard]] PlayerResult scan(ScanDirection direction);
  [[nodiscard]] PlayerResult slow(ScanDirection direction);
  [[nodiscard]] PlayerResult end_scan();
  [[nodiscard]] PlayerResult update_position(MediaTime position);

  [[nodiscard]] PlayerResult set_repeat(RepeatMode mode);
  [[nodiscard]] PlayerResult mark_ab_point();
  [[nodiscard]] PlayerResult set_ab_points(MediaTime point_a,
                                           MediaTime point_b);
  [[nodiscard]] PlayerResult clear_ab_repeat();
  [[nodiscard]] PlayerResult set_shuffle(bool enabled);
  [[nodiscard]] PlayerResult set_volume_level(std::int8_t level);
  [[nodiscard]] PlayerResult set_muted(bool muted);

  [[nodiscard]] PlayerResult set_screen_mode(ScreenMode mode);
  [[nodiscard]] PlayerResult set_zoom_pan(ZoomPanState zoom_pan);
  [[nodiscard]] PlayerResult set_panel(PlayerPanel panel);
  [[nodiscard]] PlayerResult set_focus(FocusTarget focus);
  [[nodiscard]] PlayerResult set_submenu(PlayerSubmenu submenu);
  [[nodiscard]] PlayerResult set_osd_visible(bool visible);
  [[nodiscard]] PlayerResult set_full_information_visible(bool visible);
  [[nodiscard]] PlayerResult set_visualizer(MusicVisualizer visualizer);

  [[nodiscard]] PlayerResult begin_buffering(std::string reason);
  [[nodiscard]] PlayerResult update_buffering(double progress);
  [[nodiscard]] PlayerResult finish_buffering();
  [[nodiscard]] PlayerResult
  report_issue(MediaError error, IssueSeverity severity, bool recoverable);
  [[nodiscard]] PlayerResult clear_issue();

  [[nodiscard]] PlayerResult
  select_subtitle(std::optional<MediaId> subtitle_id);
  [[nodiscard]] PlayerResult select_audio(MediaId audio_id);
  [[nodiscard]] PlayerResult set_audio_channel(AudioChannelMode mode);
  [[nodiscard]] PlayerResult set_codec_badges(std::vector<CodecBadge> badges);
  [[nodiscard]] PlayerResult set_enhancement(Enhancement enhancement,
                                             bool enabled);

private:
  [[nodiscard]] PlayerResult require_loaded(const char *operation) const;
  [[nodiscard]] PlayerResult transition(PlayerStatus previous_status,
                                        MediaTime previous_position,
                                        PlayerEffect effect,
                                        std::string detail = {}) const;
  [[nodiscard]] PlayerResult failure(MediaErrorCode code, const char *operation,
                                     std::string detail) const;
  [[nodiscard]] bool contains_subtitle(const MediaId &id) const noexcept;
  [[nodiscard]] bool contains_audio(const MediaId &id) const noexcept;

  PlayerState state_;
  PlayerStatus status_before_buffering_{PlayerStatus::paused};
  PlayerStatus status_before_issue_{PlayerStatus::stopped};
};

} // namespace openxmb::media
