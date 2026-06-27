#include "openxmb/media/player_state.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>

namespace openxmb::media {
namespace {

constexpr auto kMinimumAbLength = std::chrono::milliseconds(500);
constexpr std::array<double, 4> kScanRates{1.5, 10.0, 30.0, 120.0};

[[nodiscard]] bool has_text(const std::string &text) {
  return std::ranges::any_of(text, [](unsigned char character) {
    return character != ' ' && character != '\t' && character != '\r' &&
           character != '\n';
  });
}

[[nodiscard]] bool valid_direct_rate(double rate) noexcept {
  return std::isfinite(rate) && (rate == 0.5 || rate == 1.0 || rate == 1.5);
}

[[nodiscard]] MediaResult<void>
validate_id_list(const std::vector<MediaId> &ids, const char *operation,
                 const char *kind) {
  std::unordered_set<std::string_view> unique;
  unique.reserve(ids.size());
  for (const auto &id : ids) {
    if (!has_text(id)) {
      return MediaResult<void>::failure(make_media_error(
          MediaErrorCode::invalid_argument, operation,
          std::string(kind) + " identifier must not be empty"));
    }
    if (!unique.insert(id).second) {
      return MediaResult<void>::failure(make_media_error(
          MediaErrorCode::duplicate_id, operation,
          std::string(kind) + " identifiers must be unique", id));
    }
  }
  return MediaResult<void>::success();
}

[[nodiscard]] bool badge_valid(const CodecBadge &badge) noexcept {
  return has_text(badge.label) && has_text(badge.semantic_icon);
}

} // namespace

PlayerResult PlayerStateMachine::failure(MediaErrorCode code,
                                         const char *operation,
                                         std::string detail) const {
  return PlayerResult::failure(
      make_media_error(code, operation, std::move(detail), state_.item_id));
}

PlayerResult PlayerStateMachine::transition(PlayerStatus previous_status,
                                            MediaTime previous_position,
                                            PlayerEffect effect,
                                            std::string detail) const {
  return PlayerResult::success({.previous_status = previous_status,
                                .current_status = state_.status,
                                .previous_position = previous_position,
                                .current_position = state_.position,
                                .effect = effect,
                                .detail = std::move(detail)});
}

PlayerResult PlayerStateMachine::require_loaded(const char *operation) const {
  if (state_.status == PlayerStatus::unloaded || !state_.media_kind) {
    return failure(MediaErrorCode::not_loaded, operation,
                   "no media item is loaded");
  }
  return transition(state_.status, state_.position, PlayerEffect::none);
}

bool PlayerStateMachine::contains_subtitle(const MediaId &id) const noexcept {
  return std::ranges::find(state_.available_subtitle_ids, id) !=
         state_.available_subtitle_ids.end();
}

bool PlayerStateMachine::contains_audio(const MediaId &id) const noexcept {
  return std::ranges::find(state_.available_audio_ids, id) !=
         state_.available_audio_ids.end();
}

PlayerResult PlayerStateMachine::load(PlayerLoadRequest request) {
  if (state_.status != PlayerStatus::unloaded) {
    return failure(MediaErrorCode::already_loaded, "load_media",
                   "unload the current item before loading another item");
  }
  if (!has_text(request.item_id)) {
    return failure(MediaErrorCode::invalid_argument, "load_media",
                   "media item identifier must not be empty");
  }
  if (request.duration && request.duration->count() <= 0) {
    return failure(MediaErrorCode::invalid_argument, "load_media",
                   "known media duration must be positive");
  }
  if (request.initial_position.count() < 0 ||
      (request.duration && request.initial_position >= *request.duration)) {
    return failure(MediaErrorCode::out_of_range, "load_media",
                   "initial position must be within the media duration");
  }
  if (auto valid = validate_id_list(request.subtitle_ids, "load_media",
                                    "subtitle track");
      !valid) {
    return PlayerResult::failure(std::move(valid.error()));
  }
  if (auto valid =
          validate_id_list(request.audio_ids, "load_media", "audio track");
      !valid) {
    return PlayerResult::failure(std::move(valid.error()));
  }
  if (request.default_subtitle_id &&
      std::ranges::find(request.subtitle_ids, *request.default_subtitle_id) ==
          request.subtitle_ids.end()) {
    return failure(MediaErrorCode::missing_reference, "load_media",
                   "default subtitle is not in the available track list");
  }
  if (request.default_audio_id &&
      std::ranges::find(request.audio_ids, *request.default_audio_id) ==
          request.audio_ids.end()) {
    return failure(MediaErrorCode::missing_reference, "load_media",
                   "default audio is not in the available track list");
  }
  if (std::ranges::find_if(request.codec_badges, [](const CodecBadge &badge) {
        return !badge_valid(badge);
      }) != request.codec_badges.end()) {
    return failure(MediaErrorCode::invalid_argument, "load_media",
                   "codec badges require labels and semantic icons");
  }

  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_ = {};
  state_.media_kind = request.media_kind;
  state_.item_id = std::move(request.item_id);
  state_.duration = request.duration;
  state_.position = request.initial_position;
  state_.status =
      request.start_playing ? PlayerStatus::playing : PlayerStatus::paused;
  state_.audio_present = request.audio_present;
  state_.available_subtitle_ids = std::move(request.subtitle_ids);
  state_.available_audio_ids = std::move(request.audio_ids);
  state_.selected_subtitle_id = std::move(request.default_subtitle_id);
  state_.selected_audio_id = std::move(request.default_audio_id);
  state_.codec_badges = std::move(request.codec_badges);
  status_before_buffering_ = state_.status;
  status_before_issue_ = state_.status;
  return transition(previous_status, previous_position,
                    PlayerEffect::decoder_load, "media loaded");
}

PlayerResult PlayerStateMachine::unload() {
  if (auto loaded = require_loaded("unload_media"); !loaded) {
    return loaded;
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_ = {};
  status_before_buffering_ = PlayerStatus::paused;
  status_before_issue_ = PlayerStatus::stopped;
  return transition(previous_status, previous_position,
                    PlayerEffect::decoder_unload, "media unloaded");
}

PlayerResult PlayerStateMachine::play() {
  if (auto loaded = require_loaded("play"); !loaded) {
    return loaded;
  }
  if (state_.status == PlayerStatus::error) {
    return failure(MediaErrorCode::invalid_state, "play",
                   "clear the fatal player issue before playback");
  }
  if (state_.status == PlayerStatus::buffering) {
    return failure(MediaErrorCode::invalid_state, "play",
                   "playback cannot start while buffering is active");
  }
  if (state_.status == PlayerStatus::playing && state_.playback_rate == 1.0) {
    return failure(MediaErrorCode::invalid_state, "play",
                   "media is already playing at normal speed");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  if (state_.status == PlayerStatus::stopped ||
      state_.status == PlayerStatus::ended) {
    state_.position = {};
  }
  state_.status = PlayerStatus::playing;
  state_.playback_rate = 1.0;
  return transition(previous_status, previous_position,
                    PlayerEffect::decoder_play, "normal playback");
}

PlayerResult PlayerStateMachine::pause() {
  if (auto loaded = require_loaded("pause"); !loaded) {
    return loaded;
  }
  if (state_.status != PlayerStatus::playing &&
      state_.status != PlayerStatus::scanning) {
    return failure(MediaErrorCode::invalid_state, "pause",
                   "only playing or scanning media can be paused");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.status = PlayerStatus::paused;
  state_.playback_rate = 1.0;
  return transition(previous_status, previous_position,
                    PlayerEffect::decoder_pause, "playback paused");
}

PlayerResult PlayerStateMachine::stop() {
  if (auto loaded = require_loaded("stop"); !loaded) {
    return loaded;
  }
  if (state_.status == PlayerStatus::error) {
    return failure(MediaErrorCode::invalid_state, "stop",
                   "clear the fatal player issue before stopping");
  }
  if (state_.status == PlayerStatus::stopped && state_.position.count() == 0) {
    return failure(MediaErrorCode::invalid_state, "stop",
                   "media is already stopped");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.status = PlayerStatus::stopped;
  state_.position = {};
  state_.playback_rate = 1.0;
  state_.buffering = {};
  return transition(previous_status, previous_position,
                    PlayerEffect::decoder_stop, "playback stopped");
}

PlayerResult PlayerStateMachine::seek(MediaTime position) {
  if (auto loaded = require_loaded("seek"); !loaded) {
    return loaded;
  }
  if (state_.status == PlayerStatus::error) {
    return failure(MediaErrorCode::invalid_state, "seek",
                   "clear the fatal player issue before seeking");
  }
  if (position.count() < 0 ||
      (state_.duration && position > *state_.duration)) {
    return failure(MediaErrorCode::out_of_range, "seek",
                   "seek position is outside the media duration");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.position = position;
  if (state_.status == PlayerStatus::scanning ||
      state_.status == PlayerStatus::ended) {
    state_.status = PlayerStatus::playing;
  }
  state_.playback_rate = 1.0;
  return transition(previous_status, previous_position,
                    PlayerEffect::decoder_seek, "absolute seek");
}

PlayerResult PlayerStateMachine::set_rate(double rate) {
  if (auto loaded = require_loaded("set_playback_rate"); !loaded) {
    return loaded;
  }
  if (!valid_direct_rate(rate)) {
    return failure(MediaErrorCode::out_of_range, "set_playback_rate",
                   "direct rate must be one of 0.5, 1.0, or 1.5");
  }
  if (state_.status == PlayerStatus::error ||
      state_.status == PlayerStatus::buffering) {
    return failure(MediaErrorCode::invalid_state, "set_playback_rate",
                   "playback rate cannot change in the current state");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.status = PlayerStatus::playing;
  state_.playback_rate = rate;
  return transition(previous_status, previous_position,
                    PlayerEffect::decoder_set_rate,
                    "direct playback rate changed");
}

PlayerResult PlayerStateMachine::scan(ScanDirection direction) {
  if (auto loaded = require_loaded("scan"); !loaded) {
    return loaded;
  }
  if (state_.status == PlayerStatus::error ||
      state_.status == PlayerStatus::buffering) {
    return failure(MediaErrorCode::invalid_state, "scan",
                   "scan cannot start in the current state");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  const double sign = direction == ScanDirection::forward ? 1.0 : -1.0;
  double magnitude = kScanRates.front();
  if (state_.status == PlayerStatus::scanning &&
      std::signbit(state_.playback_rate) == std::signbit(sign)) {
    const auto current = std::abs(state_.playback_rate);
    const auto found = std::ranges::find(kScanRates, current);
    if (found != kScanRates.end() && std::next(found) != kScanRates.end()) {
      magnitude = *std::next(found);
    }
  }
  state_.status = PlayerStatus::scanning;
  state_.playback_rate = sign * magnitude;
  return transition(
      previous_status, previous_position, PlayerEffect::decoder_scan,
      direction == ScanDirection::forward ? "fast forward" : "fast reverse");
}

PlayerResult PlayerStateMachine::slow(ScanDirection direction) {
  if (auto loaded = require_loaded("slow_playback"); !loaded) {
    return loaded;
  }
  if (state_.status == PlayerStatus::error ||
      state_.status == PlayerStatus::buffering) {
    return failure(MediaErrorCode::invalid_state, "slow_playback",
                   "slow playback cannot start in the current state");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.status = direction == ScanDirection::forward ? PlayerStatus::playing
                                                      : PlayerStatus::scanning;
  state_.playback_rate = direction == ScanDirection::forward ? 0.5 : -0.5;
  return transition(
      previous_status, previous_position,
      direction == ScanDirection::forward ? PlayerEffect::decoder_set_rate
                                          : PlayerEffect::decoder_scan,
      direction == ScanDirection::forward ? "slow forward" : "slow reverse");
}

PlayerResult PlayerStateMachine::end_scan() {
  if (auto loaded = require_loaded("end_scan"); !loaded) {
    return loaded;
  }
  if (state_.status != PlayerStatus::scanning) {
    return failure(MediaErrorCode::invalid_state, "end_scan",
                   "the player is not scanning");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.status = PlayerStatus::playing;
  state_.playback_rate = 1.0;
  return transition(previous_status, previous_position,
                    PlayerEffect::decoder_play, "scan ended");
}

PlayerResult PlayerStateMachine::update_position(MediaTime position) {
  if (auto loaded = require_loaded("update_position"); !loaded) {
    return loaded;
  }
  if (state_.status == PlayerStatus::error) {
    return failure(MediaErrorCode::invalid_state, "update_position",
                   "a fatal player issue is active");
  }
  if (position.count() < 0) {
    return failure(MediaErrorCode::out_of_range, "update_position",
                   "reported position must be non-negative");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;

  if (state_.repeat == RepeatMode::a_b && state_.repeat_section.point_a &&
      state_.repeat_section.point_b &&
      position >= *state_.repeat_section.point_b) {
    state_.position = *state_.repeat_section.point_a;
    return transition(previous_status, previous_position,
                      PlayerEffect::decoder_seek, "A-B repeat wrapped");
  }

  if (state_.duration && position >= *state_.duration) {
    switch (state_.repeat) {
    case RepeatMode::one:
    case RepeatMode::title:
      state_.position = {};
      state_.status = PlayerStatus::playing;
      state_.playback_rate = 1.0;
      return transition(previous_status, previous_position,
                        PlayerEffect::decoder_seek, "title repeat wrapped");
    case RepeatMode::all:
    case RepeatMode::folder:
      state_.position = *state_.duration;
      state_.status = PlayerStatus::ended;
      state_.playback_rate = 1.0;
      return transition(previous_status, previous_position,
                        PlayerEffect::advance_item,
                        "collection repeat requested the next item");
    case RepeatMode::off:
    case RepeatMode::a_b:
      state_.position = *state_.duration;
      state_.status = PlayerStatus::ended;
      state_.playback_rate = 1.0;
      return transition(
          previous_status, previous_position, PlayerEffect::advance_item,
          "title ended; coordinator decides whether a next item exists");
    }
  }

  state_.position = position;
  return transition(previous_status, previous_position, PlayerEffect::none,
                    "position updated");
}

PlayerResult PlayerStateMachine::set_repeat(RepeatMode mode) {
  if (auto loaded = require_loaded("set_repeat"); !loaded) {
    return loaded;
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.repeat = mode;
  if (mode != RepeatMode::a_b) {
    state_.repeat_section = {};
  }
  return transition(previous_status, previous_position, PlayerEffect::ui_only,
                    "repeat mode changed");
}

PlayerResult PlayerStateMachine::mark_ab_point() {
  if (auto loaded = require_loaded("mark_ab_point"); !loaded) {
    return loaded;
  }
  if (state_.repeat != RepeatMode::a_b) {
    return failure(MediaErrorCode::invalid_state, "mark_ab_point",
                   "select A-B repeat before marking points");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  if (!state_.repeat_section.point_a) {
    state_.repeat_section.point_a = state_.position;
    return transition(previous_status, previous_position, PlayerEffect::ui_only,
                      "A-B point A set");
  }
  if (!state_.repeat_section.point_b) {
    if (state_.position < *state_.repeat_section.point_a + kMinimumAbLength) {
      return failure(MediaErrorCode::out_of_range, "mark_ab_point",
                     "point B must be at least 500 ms after point A");
    }
    state_.repeat_section.point_b = state_.position;
    return transition(previous_status, previous_position, PlayerEffect::ui_only,
                      "A-B point B set");
  }
  state_.repeat = RepeatMode::off;
  state_.repeat_section = {};
  return transition(previous_status, previous_position, PlayerEffect::ui_only,
                    "A-B repeat cleared");
}

PlayerResult PlayerStateMachine::set_ab_points(MediaTime point_a,
                                               MediaTime point_b) {
  if (auto loaded = require_loaded("set_ab_points"); !loaded) {
    return loaded;
  }
  if (point_a.count() < 0 || point_b < point_a + kMinimumAbLength ||
      (state_.duration && point_b > *state_.duration)) {
    return failure(MediaErrorCode::out_of_range, "set_ab_points",
                   "A-B points must form a valid section within the title");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.repeat = RepeatMode::a_b;
  state_.repeat_section = {.point_a = point_a, .point_b = point_b};
  return transition(previous_status, previous_position, PlayerEffect::ui_only,
                    "A-B repeat section set");
}

PlayerResult PlayerStateMachine::clear_ab_repeat() {
  if (auto loaded = require_loaded("clear_ab_repeat"); !loaded) {
    return loaded;
  }
  if (state_.repeat != RepeatMode::a_b && !state_.repeat_section.point_a &&
      !state_.repeat_section.point_b) {
    return failure(MediaErrorCode::invalid_state, "clear_ab_repeat",
                   "A-B repeat is not active");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.repeat = RepeatMode::off;
  state_.repeat_section = {};
  return transition(previous_status, previous_position, PlayerEffect::ui_only,
                    "A-B repeat cleared");
}

PlayerResult PlayerStateMachine::set_shuffle(bool enabled) {
  if (auto loaded = require_loaded("set_shuffle"); !loaded) {
    return loaded;
  }
  if (state_.media_kind != MediaKind::music) {
    return failure(MediaErrorCode::unsupported_operation, "set_shuffle",
                   "shuffle is available only for music playback");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.shuffle = enabled;
  return transition(previous_status, previous_position, PlayerEffect::ui_only,
                    enabled ? "shuffle enabled" : "shuffle disabled");
}

PlayerResult PlayerStateMachine::set_volume_level(std::int8_t level) {
  if (auto loaded = require_loaded("set_volume"); !loaded) {
    return loaded;
  }
  if (!state_.audio_present) {
    return failure(MediaErrorCode::unsupported_operation, "set_volume",
                   "the loaded item has no audio stream");
  }
  if (level < -4 || level > 4) {
    return failure(MediaErrorCode::out_of_range, "set_volume",
                   "volume level must be between -4 and +4");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.volume_level = level;
  return transition(previous_status, previous_position,
                    PlayerEffect::decoder_set_volume, "volume changed");
}

PlayerResult PlayerStateMachine::set_muted(bool muted) {
  if (auto loaded = require_loaded("set_muted"); !loaded) {
    return loaded;
  }
  if (!state_.audio_present) {
    return failure(MediaErrorCode::unsupported_operation, "set_muted",
                   "the loaded item has no audio stream");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.muted = muted;
  return transition(previous_status, previous_position,
                    PlayerEffect::decoder_set_volume,
                    muted ? "audio muted" : "audio unmuted");
}

PlayerResult PlayerStateMachine::set_screen_mode(ScreenMode mode) {
  if (auto loaded = require_loaded("set_screen_mode"); !loaded) {
    return loaded;
  }
  if (state_.media_kind == MediaKind::music) {
    return failure(MediaErrorCode::unsupported_operation, "set_screen_mode",
                   "screen modes apply only to photo and video media");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.screen_mode = mode;
  state_.zoom_pan = {};
  return transition(previous_status, previous_position,
                    PlayerEffect::renderer_reconfigure,
                    "screen mode changed and zoom/pan reset");
}

PlayerResult PlayerStateMachine::set_zoom_pan(ZoomPanState zoom_pan) {
  if (auto loaded = require_loaded("set_zoom_pan"); !loaded) {
    return loaded;
  }
  if (state_.media_kind == MediaKind::music) {
    return failure(MediaErrorCode::unsupported_operation, "set_zoom_pan",
                   "zoom and pan apply only to photo and video media");
  }
  if (!std::isfinite(zoom_pan.zoom) || !std::isfinite(zoom_pan.pan_x) ||
      !std::isfinite(zoom_pan.pan_y) || zoom_pan.zoom < 1.0 ||
      zoom_pan.zoom > 8.0 || std::abs(zoom_pan.pan_x) > 1.0 ||
      std::abs(zoom_pan.pan_y) > 1.0) {
    return failure(MediaErrorCode::out_of_range, "set_zoom_pan",
                   "zoom must be 1..8 and normalized pan must be -1..1");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.zoom_pan = zoom_pan;
  return transition(previous_status, previous_position,
                    PlayerEffect::renderer_reconfigure, "zoom and pan changed");
}

PlayerResult PlayerStateMachine::set_panel(PlayerPanel panel) {
  if (auto loaded = require_loaded("set_panel"); !loaded) {
    return loaded;
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.panel = panel;
  state_.submenu = PlayerSubmenu::none;
  state_.focus =
      panel == PlayerPanel::none ? FocusTarget::media : FocusTarget::panel;
  return transition(previous_status, previous_position, PlayerEffect::ui_only,
                    "panel changed");
}

PlayerResult PlayerStateMachine::set_focus(FocusTarget focus) {
  if (auto loaded = require_loaded("set_focus"); !loaded) {
    return loaded;
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.focus = focus;
  return transition(previous_status, previous_position, PlayerEffect::ui_only,
                    "focus changed");
}

PlayerResult PlayerStateMachine::set_submenu(PlayerSubmenu submenu) {
  if (auto loaded = require_loaded("set_submenu"); !loaded) {
    return loaded;
  }
  if (submenu != PlayerSubmenu::none && state_.panel == PlayerPanel::none) {
    return failure(MediaErrorCode::invalid_state, "set_submenu",
                   "a submenu requires an open player panel");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.submenu = submenu;
  state_.focus = submenu == PlayerSubmenu::none ? FocusTarget::panel
                                                : FocusTarget::submenu;
  return transition(previous_status, previous_position, PlayerEffect::ui_only,
                    "submenu changed");
}

PlayerResult PlayerStateMachine::set_osd_visible(bool visible) {
  if (auto loaded = require_loaded("set_osd_visible"); !loaded) {
    return loaded;
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.osd_visible = visible;
  return transition(previous_status, previous_position, PlayerEffect::ui_only,
                    "on-screen display visibility changed");
}

PlayerResult PlayerStateMachine::set_full_information_visible(bool visible) {
  if (auto loaded = require_loaded("set_full_information_visible"); !loaded) {
    return loaded;
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.full_information_visible = visible;
  return transition(previous_status, previous_position, PlayerEffect::ui_only,
                    "full information visibility changed");
}

PlayerResult PlayerStateMachine::set_visualizer(MusicVisualizer visualizer) {
  if (auto loaded = require_loaded("set_visualizer"); !loaded) {
    return loaded;
  }
  if (state_.media_kind != MediaKind::music) {
    return failure(MediaErrorCode::unsupported_operation, "set_visualizer",
                   "visualizers are available only for music playback");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.visualizer = visualizer;
  return transition(previous_status, previous_position,
                    PlayerEffect::renderer_reconfigure,
                    "music visualizer changed");
}

PlayerResult PlayerStateMachine::begin_buffering(std::string reason) {
  if (auto loaded = require_loaded("begin_buffering"); !loaded) {
    return loaded;
  }
  if (state_.status == PlayerStatus::buffering) {
    return failure(MediaErrorCode::invalid_state, "begin_buffering",
                   "buffering is already active");
  }
  if (state_.status == PlayerStatus::error) {
    return failure(MediaErrorCode::invalid_state, "begin_buffering",
                   "buffering cannot begin while a fatal issue is active");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  status_before_buffering_ = state_.status;
  state_.status = PlayerStatus::buffering;
  state_.buffering = {
      .active = true, .progress = std::nullopt, .reason = std::move(reason)};
  return transition(previous_status, previous_position, PlayerEffect::ui_only,
                    "buffering began");
}

PlayerResult PlayerStateMachine::update_buffering(double progress) {
  if (state_.status != PlayerStatus::buffering || !state_.buffering.active) {
    return failure(MediaErrorCode::invalid_state, "update_buffering",
                   "buffering is not active");
  }
  if (!std::isfinite(progress) || progress < 0.0 || progress > 1.0) {
    return failure(MediaErrorCode::out_of_range, "update_buffering",
                   "buffering progress must be between zero and one");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.buffering.progress = progress;
  return transition(previous_status, previous_position, PlayerEffect::ui_only,
                    "buffering progress changed");
}

PlayerResult PlayerStateMachine::finish_buffering() {
  if (state_.status != PlayerStatus::buffering || !state_.buffering.active) {
    return failure(MediaErrorCode::invalid_state, "finish_buffering",
                   "buffering is not active");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.status = status_before_buffering_;
  state_.buffering = {};
  return transition(previous_status, previous_position,
                    state_.status == PlayerStatus::playing
                        ? PlayerEffect::decoder_play
                        : PlayerEffect::ui_only,
                    "buffering finished");
}

PlayerResult PlayerStateMachine::report_issue(MediaError error,
                                              IssueSeverity severity,
                                              bool recoverable) {
  if (auto loaded = require_loaded("report_issue"); !loaded) {
    return loaded;
  }
  if (!has_text(error.detail)) {
    return failure(MediaErrorCode::invalid_argument, "report_issue",
                   "player issue detail must not be empty");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  if (severity == IssueSeverity::fatal) {
    status_before_issue_ = state_.status;
    state_.status = PlayerStatus::error;
  }
  state_.issue = PlayerIssue{severity, std::move(error), recoverable};
  return transition(previous_status, previous_position, PlayerEffect::ui_only,
                    severity == IssueSeverity::fatal ? "fatal player issue"
                                                     : "degraded player issue");
}

PlayerResult PlayerStateMachine::clear_issue() {
  if (auto loaded = require_loaded("clear_issue"); !loaded) {
    return loaded;
  }
  if (!state_.issue) {
    return failure(MediaErrorCode::invalid_state, "clear_issue",
                   "no player issue is active");
  }
  if (!state_.issue->recoverable) {
    return failure(MediaErrorCode::unsupported_operation, "clear_issue",
                   "the active player issue is not recoverable");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  if (state_.issue->severity == IssueSeverity::fatal) {
    switch (status_before_issue_) {
    case PlayerStatus::playing:
    case PlayerStatus::scanning:
    case PlayerStatus::buffering:
      state_.status = PlayerStatus::paused;
      break;
    case PlayerStatus::error:
    case PlayerStatus::unloaded:
      state_.status = PlayerStatus::stopped;
      break;
    default:
      state_.status = status_before_issue_;
      break;
    }
  }
  state_.issue.reset();
  return transition(previous_status, previous_position,
                    PlayerEffect::recover_backend, "player issue cleared");
}

PlayerResult
PlayerStateMachine::select_subtitle(std::optional<MediaId> subtitle_id) {
  if (auto loaded = require_loaded("select_subtitle"); !loaded) {
    return loaded;
  }
  if (subtitle_id && !contains_subtitle(*subtitle_id)) {
    return failure(MediaErrorCode::missing_reference, "select_subtitle",
                   "subtitle track is not available for this item");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.selected_subtitle_id = std::move(subtitle_id);
  return transition(previous_status, previous_position,
                    PlayerEffect::decoder_select_subtitle,
                    state_.selected_subtitle_id ? "subtitle track selected"
                                                : "subtitles disabled");
}

PlayerResult PlayerStateMachine::select_audio(MediaId audio_id) {
  if (auto loaded = require_loaded("select_audio"); !loaded) {
    return loaded;
  }
  if (!contains_audio(audio_id)) {
    return failure(MediaErrorCode::missing_reference, "select_audio",
                   "audio track is not available for this item");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.selected_audio_id = std::move(audio_id);
  state_.audio_present = true;
  return transition(previous_status, previous_position,
                    PlayerEffect::decoder_select_audio, "audio track selected");
}

PlayerResult PlayerStateMachine::set_audio_channel(AudioChannelMode mode) {
  if (auto loaded = require_loaded("set_audio_channel"); !loaded) {
    return loaded;
  }
  if (!state_.audio_present) {
    return failure(MediaErrorCode::unsupported_operation, "set_audio_channel",
                   "the loaded item has no audio stream");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.audio_channel = mode;
  return transition(previous_status, previous_position,
                    PlayerEffect::decoder_select_audio,
                    "audio channel mode changed");
}

PlayerResult
PlayerStateMachine::set_codec_badges(std::vector<CodecBadge> badges) {
  if (auto loaded = require_loaded("set_codec_badges"); !loaded) {
    return loaded;
  }
  if (std::ranges::find_if(badges, [](const CodecBadge &badge) {
        return !badge_valid(badge);
      }) != badges.end()) {
    return failure(MediaErrorCode::invalid_argument, "set_codec_badges",
                   "codec badges require labels and semantic icons");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  state_.codec_badges = std::move(badges);
  return transition(previous_status, previous_position, PlayerEffect::ui_only,
                    "codec badges changed");
}

PlayerResult PlayerStateMachine::set_enhancement(Enhancement enhancement,
                                                 bool enabled) {
  if (auto loaded = require_loaded("set_enhancement"); !loaded) {
    return loaded;
  }
  if (state_.media_kind != MediaKind::video) {
    return failure(MediaErrorCode::unsupported_operation, "set_enhancement",
                   "AV enhancements are available only for video playback");
  }
  const auto previous_status = state_.status;
  const auto previous_position = state_.position;
  switch (enhancement) {
  case Enhancement::block_noise_reduction:
    state_.enhancements.block_noise_reduction = enabled;
    break;
  case Enhancement::frame_noise_reduction:
    state_.enhancements.frame_noise_reduction = enabled;
    break;
  case Enhancement::mosquito_noise_reduction:
    state_.enhancements.mosquito_noise_reduction = enabled;
    break;
  case Enhancement::upscale:
    state_.enhancements.upscale = enabled;
    break;
  }
  return transition(previous_status, previous_position,
                    PlayerEffect::renderer_reconfigure,
                    "video enhancement changed");
}

} // namespace openxmb::media
