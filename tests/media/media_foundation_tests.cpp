#include "openxmb/media/catalog.hpp"
#include "openxmb/media/clock.hpp"
#include "openxmb/media/player_state.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace std::chrono_literals;
using namespace openxmb::media;

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

template <typename T>
void require(const MediaResult<T> &result, std::string_view message) {
  require(result.has_value(), message);
}

template <typename Actual, typename Expected>
void require_equal(const Actual &actual, const Expected &expected,
                   std::string_view message) {
  if (!(actual == expected)) {
    throw std::runtime_error(std::string(message));
  }
}

void require_near(double actual, double expected, double tolerance,
                  std::string_view message) {
  if (std::abs(actual - expected) > tolerance) {
    throw std::runtime_error(std::string(message) + ": expected " +
                             std::to_string(expected) + ", got " +
                             std::to_string(actual));
  }
}

class ManualMonotonicClock final : public IMonotonicClock {
public:
  [[nodiscard]] MediaTime now() const noexcept override { return now_; }
  void advance(MediaTime duration) noexcept { now_ += duration; }
  void set(MediaTime time) noexcept { now_ = time; }

private:
  MediaTime now_{};
};

class ManualAudioClock final : public IAudioClock {
public:
  [[nodiscard]] MediaResult<AudioClockReading> read() const override {
    if (failure_) {
      return MediaResult<AudioClockReading>::failure(
          make_media_error(MediaErrorCode::backend_failure,
                           "manual_audio_clock", "injected backend failure"));
    }
    return MediaResult<AudioClockReading>::success(reading_);
  }

  void set(MediaTime position, std::uint64_t sequence, bool running = true) {
    reading_ = {.position = position, .sequence = sequence, .running = running};
  }
  void fail(bool enabled) noexcept { failure_ = enabled; }

private:
  AudioClockReading reading_{};
  bool failure_{false};
};

void test_media_time_conversion() {
  const auto converted = media_time_from_seconds(1.25);
  require(converted, "finite seconds convert to media time");
  require_equal(converted.value(), 1250ms, "seconds conversion is exact");
  require_near(media_time_seconds(converted.value()), 1.25, 1e-12,
               "media time converts back to seconds");
  require(!media_time_from_seconds(-1.0), "negative seconds are rejected");
  require(!media_time_from_seconds(std::nan("")),
          "non-finite seconds are rejected");
}

void test_monotonic_playback_clock() {
  ManualMonotonicClock monotonic;
  PlaybackClock clock(monotonic);
  const auto before_configuration = clock.sample();
  require(!before_configuration, "unconfigured clock sample is rejected");
  require_equal(before_configuration.error().code,
                MediaErrorCode::invalid_state,
                "unconfigured clock returns a typed state error");

  auto configured = clock.configure({.initial_position = 10s,
                                     .playback_rate = 1.0,
                                     .audio_present = false,
                                     .start_playing = true});
  require(configured, "silent media configures with no audio service");
  require_equal(configured.value().master, ClockMaster::monotonic,
                "silent media uses monotonic master");

  monotonic.advance(2s);
  auto sample = clock.sample();
  require(sample, "monotonic sample succeeds");
  require_equal(sample.value().position, 12s,
                "monotonic clock advances at normal rate");

  auto rate = clock.set_rate(2.0);
  require(rate, "clock accepts a 2x backend rate");
  monotonic.advance(1500ms);
  sample = clock.sample();
  require_equal(sample.value().position, 15s,
                "clock applies rate only after the rate anchor");

  auto paused = clock.pause();
  require(paused && !paused.value().playing, "pause returns a paused snapshot");
  monotonic.advance(5s);
  sample = clock.sample();
  require_equal(sample.value().position, 15s,
                "paused monotonic clock does not drift");
  require(!clock.pause(), "repeated pause is an explicit invalid transition");

  auto seek = clock.seek(33s);
  require(seek && seek.value().discontinuity,
          "seek reports a local discontinuity");
  require_equal(seek.value().position, 33s, "paused seek is exact");
  auto resumed = clock.resume();
  require(resumed && resumed.value().playing, "resume restarts the timeline");
  monotonic.advance(500ms);
  sample = clock.sample();
  require_equal(sample.value().position, 34s,
                "resumed clock retains its configured 2x rate");

  monotonic.set(1ns);
  const auto backwards = clock.sample();
  require(!backwards, "backwards monotonic time is rejected");
  require_equal(backwards.error().code, MediaErrorCode::clock_discontinuity,
                "backwards time reports a clock discontinuity");
}

void test_audio_master_clock_and_discontinuities() {
  ManualMonotonicClock monotonic;
  ManualAudioClock audio;
  audio.set(5s, 7);
  PlaybackClock clock(monotonic, &audio);
  auto configured = clock.configure({.initial_position = {},
                                     .playback_rate = 1.0,
                                     .audio_present = true,
                                     .start_playing = true,
                                     .discontinuity_threshold = 200ms});
  require(configured, "audio-backed clock configures");
  require_equal(configured.value().master, ClockMaster::audio,
                "audio is the playback master when present");
  require_equal(configured.value().position, 5s,
                "audio timestamp wins over the requested anchor");

  monotonic.advance(100ms);
  audio.set(5100ms, 7);
  auto sample = clock.sample();
  require(sample && !sample.value().discontinuity,
          "locked audio and monotonic clocks do not report drift");
  require_equal(sample.value().audio_drift, 0ns,
                "locked clocks report zero drift");

  monotonic.advance(100ms);
  audio.set(9s, 7);
  sample = clock.sample();
  require(sample && sample.value().discontinuity,
          "large audio drift is reported as a discontinuity");
  require_equal(sample.value().master, ClockMaster::audio,
                "audio remains master across drift correction");
  const auto drift_generation = sample.value().discontinuity_generation;

  monotonic.advance(10ms);
  audio.set(9010ms, 8);
  sample = clock.sample();
  require(sample && sample.value().discontinuity,
          "audio sequence changes are discontinuities");
  require(sample.value().discontinuity_generation > drift_generation,
          "external discontinuity increments the generation");

  const auto seek = clock.seek(40s);
  require(seek && seek.value().master == ClockMaster::monotonic_pending_audio,
          "seek waits for audio sequence acknowledgement");
  monotonic.advance(50ms);
  audio.set(9060ms, 8);
  sample = clock.sample();
  require(sample &&
              sample.value().master == ClockMaster::monotonic_pending_audio,
          "stale post-seek audio cannot undo the seek");
  require_equal(sample.value().position, 40050ms,
                "pending audio uses deterministic monotonic projection");

  audio.set(40050ms, 9);
  sample = clock.sample();
  require(sample && sample.value().master == ClockMaster::audio,
          "new audio sequence acknowledges the seek");
  require(sample.value().discontinuity,
          "seek acknowledgement remains visible to consumers");

  audio.fail(true);
  const auto failed = clock.sample();
  require(!failed, "audio backend failure is not silently swallowed");
  require_equal(failed.error().code, MediaErrorCode::backend_failure,
                "audio backend failure remains typed");
  audio.fail(false);

  auto silent = clock.set_audio_present(false);
  require(silent && silent.value().master == ClockMaster::monotonic,
          "audio removal switches to monotonic master");
  monotonic.advance(1s);
  sample = clock.sample();
  require_equal(sample.value().position, 41050ms,
                "monotonic fallback remains continuous after audio removal");

  ManualMonotonicClock paused_monotonic;
  ManualAudioClock stale_audio;
  stale_audio.set(5s, 0, false);
  PlaybackClock paused_clock(paused_monotonic, &stale_audio);
  require(paused_clock.configure({.initial_position = {},
                                  .playback_rate = 1.0,
                                  .audio_present = true,
                                  .start_playing = false}),
          "paused audio-backed clock configures without consuming audio");
  require(paused_clock.seek(40s), "paused audio clock accepts a seek");
  auto pending_resume = paused_clock.resume();
  require(
      pending_resume &&
          pending_resume.value().master == ClockMaster::monotonic_pending_audio,
      "unknown stale audio cannot undo a seek made before the first sample");
  stale_audio.set(40s, 1, true);
  auto synchronised = paused_clock.sample();
  require(synchronised && synchronised.value().master == ClockMaster::audio,
          "first audio sequence is accepted once it reaches the seek anchor");
}

CatalogInput representative_catalog() {
  CatalogInput input;
  input.photos.push_back(
      {.id = "photo.sunrise",
       .title = "Sunrise",
       .source_uri = "file:///media/sunrise.jpg",
       .thumbnail =
           ImageReference{.uri = "file:///cache/sunrise-thumb.jpg",
                          .pixel_width = 320,
                          .pixel_height = 180,
                          .accessible_description = "Sunrise thumbnail"},
       .captured_at = std::chrono::sys_seconds(1'700'000'000s),
       .camera_model = "Camera",
       .tags = {"sunrise"},
       .group_ids = {"photos.album.holiday"},
       .orientation_degrees = 0,
       .pixel_width = 1920,
       .pixel_height = 1080});
  input.photo_groups.push_back({.id = "photos.album.holiday",
                                .title = "Holiday",
                                .kind = PhotoGroupKind::album,
                                .photo_ids = {"photo.sunrise"},
                                .cover_photo_id = "photo.sunrise"});

  input.artists.push_back({.id = "artist.example",
                           .name = "Example Artist",
                           .album_ids = {"album.example"},
                           .portrait = std::nullopt});
  input.albums.push_back({.id = "album.example",
                          .title = "Example Album",
                          .artist_ids = {"artist.example"},
                          .track_ids = {"track.example"},
                          .artwork = std::nullopt,
                          .release_year = 2026,
                          .genre = "Electronic"});
  input.tracks.push_back(
      {.id = "track.example",
       .title = "Example Track",
       .source_uri = "file:///media/example.flac",
       .artist_ids = {"artist.example"},
       .album_id = "album.example",
       .artwork = std::nullopt,
       .duration = 215s,
       .disc_number = 1,
       .track_number = 1,
       .genre = "Electronic",
       .technical = {.container = "FLAC",
                     .video_codec = {},
                     .audio_codec = "FLAC",
                     .bit_rate = 900'000,
                     .pixel_width = 0,
                     .pixel_height = 0,
                     .sample_rate = 48'000,
                     .channel_count = 2,
                     .badges = {{.kind = CodecKind::audio,
                                 .label = "FLAC",
                                 .semantic_icon = "codec.audio.flac"}}}});
  input.playlists.push_back({.id = "playlist.favourites",
                             .title = "Favourites",
                             .track_ids = {"track.example"},
                             .artwork = std::nullopt,
                             .user_editable = true});

  input.videos.push_back(
      {.id = "video.example",
       .title = "Example Video",
       .source_uri = "file:///media/example.mkv",
       .poster = ImageReference{.uri = "file:///media/poster.jpg",
                                .pixel_width = 640,
                                .pixel_height = 360,
                                .accessible_description = "Video poster"},
       .duration = 120s,
       .chapters = {{.id = "chapter.opening",
                     .title = "Opening",
                     .start = 0s,
                     .thumbnail = std::nullopt},
                    {.id = "chapter.middle",
                     .title = "Middle",
                     .start = 60s,
                     .thumbnail = std::nullopt}},
       .subtitle_tracks = {{.id = "subtitle.en",
                            .label = "English",
                            .language = "en-GB",
                            .external_uri = std::nullopt,
                            .backend_stream_key = "subtitle:0",
                            .is_default = true,
                            .forced = false,
                            .closed_captions = false}},
       .audio_tracks = {{.id = "audio.en",
                         .label = "English 2 Ch.",
                         .language = "en-GB",
                         .external_uri = std::nullopt,
                         .backend_stream_key = "audio:0",
                         .codec = "AAC",
                         .channel_count = 2,
                         .is_default = true,
                         .audio_description = false}},
       .resume =
           ResumePoint{.position = 30s,
                       .updated_at = std::chrono::sys_seconds(1'700'000'100s)},
       .technical = {.container = "Matroska",
                     .video_codec = "AVC",
                     .audio_codec = "AAC",
                     .bit_rate = 4'000'000,
                     .pixel_width = 1920,
                     .pixel_height = 1080,
                     .sample_rate = 48'000,
                     .channel_count = 2,
                     .badges = {{.kind = CodecKind::video,
                                 .label = "AVC",
                                 .semantic_icon = "codec.video.avc"},
                                {.kind = CodecKind::audio,
                                 .label = "AAC",
                                 .semantic_icon = "codec.audio.aac"}}},
       .folder_id = "videos.folder.samples",
       .recorded_at = std::nullopt});
  return input;
}

void test_catalog_validation_and_lookup() {
  auto input = representative_catalog();
  auto catalog = Catalog::create(std::move(input));
  require(catalog, "representative photo/music/video catalog validates");
  require(catalog.value().find_photo("photo.sunrise") != nullptr,
          "photo lookup uses stable identifier");
  require(catalog.value().find_track("track.example") != nullptr,
          "music track lookup uses stable identifier");
  require(catalog.value().find_video("video.example") != nullptr,
          "video lookup uses stable identifier");
  require(catalog.value().find_video("missing") == nullptr,
          "missing lookup is explicit null");
  require_equal(catalog.value().videos().front().chapters.size(),
                std::size_t{2}, "video chapters survive catalog creation");
}

void test_malformed_catalog_inputs() {
  {
    auto input = representative_catalog();
    input.tracks.push_back(input.tracks.front());
    const auto result = Catalog::create(std::move(input));
    require(!result, "duplicate media identifiers are rejected");
    require_equal(result.error().code, MediaErrorCode::duplicate_id,
                  "duplicate identifiers have a typed error");
  }
  {
    auto input = representative_catalog();
    input.playlists.front().track_ids = {"track.missing"};
    const auto result = Catalog::create(std::move(input));
    require(!result, "missing playlist reference is rejected");
    require_equal(result.error().code, MediaErrorCode::missing_reference,
                  "missing relationship has a typed error");
  }
  {
    auto input = representative_catalog();
    input.photos.front().group_ids.clear();
    input.photo_groups.front().cover_photo_id = "photo.missing";
    const auto result = Catalog::create(std::move(input));
    require(!result, "cover outside a photo group is rejected");
  }
  {
    auto input = representative_catalog();
    input.photos.front().group_ids.clear();
    input.photo_groups.front().cover_photo_id.reset();
    const auto result = Catalog::create(std::move(input));
    require(!result, "asymmetric photo-group membership is rejected");
    require_equal(result.error().code, MediaErrorCode::missing_reference,
                  "asymmetric membership has a typed relationship error");
  }
  {
    auto input = representative_catalog();
    input.videos.front().chapters[1].start = 0s;
    const auto result = Catalog::create(std::move(input));
    require(!result, "non-increasing video chapters are rejected");
    require_equal(result.error().code, MediaErrorCode::malformed_catalog,
                  "malformed chapters have a typed error");
  }
  {
    auto input = representative_catalog();
    input.videos.front().subtitle_tracks.push_back(
        {.id = "subtitle.fr",
         .label = "French",
         .language = "fr",
         .external_uri = "file:///media/example.fr.srt",
         .backend_stream_key = {},
         .is_default = true,
         .forced = false,
         .closed_captions = false});
    const auto result = Catalog::create(std::move(input));
    require(!result, "multiple default subtitle tracks are rejected");
  }
  {
    auto input = representative_catalog();
    input.videos.front().resume->position = 120s;
    const auto result = Catalog::create(std::move(input));
    require(!result, "resume point at title end is rejected");
  }
}

PlayerLoadRequest music_request() {
  return {.media_kind = MediaKind::music,
          .item_id = "track.example",
          .duration = 215s,
          .initial_position = 5s,
          .audio_present = true,
          .start_playing = true,
          .subtitle_ids = {},
          .audio_ids = {"audio.main"},
          .default_subtitle_id = std::nullopt,
          .default_audio_id = "audio.main",
          .codec_badges = {{.kind = CodecKind::audio,
                            .label = "FLAC",
                            .semantic_icon = "codec.audio.flac"}}};
}

PlayerLoadRequest video_request() {
  return {.media_kind = MediaKind::video,
          .item_id = "video.example",
          .duration = 100s,
          .initial_position = {},
          .audio_present = true,
          .start_playing = true,
          .subtitle_ids = {"subtitle.en", "subtitle.ja"},
          .audio_ids = {"audio.en", "audio.ja"},
          .default_subtitle_id = "subtitle.en",
          .default_audio_id = "audio.en",
          .codec_badges = {{.kind = CodecKind::video,
                            .label = "AVC",
                            .semantic_icon = "codec.video.avc"}}};
}

void test_music_player_states_and_scanning() {
  PlayerStateMachine player;
  require(!player.pause(), "unloaded pause is rejected");

  auto loaded = player.load(music_request());
  require(loaded && loaded.value().effect == PlayerEffect::decoder_load,
          "music load requests decoder setup");
  require_equal(player.state().status, PlayerStatus::playing,
                "music starts playing");
  require_equal(player.state().visualizer, MusicVisualizer::xmb_waves,
                "XMB Waves is the default music visualizer");

  require(player.set_visualizer(MusicVisualizer::canyon),
          "music visualizer can change");
  require(player.set_shuffle(true), "music shuffle can be enabled");
  require(player.set_repeat(RepeatMode::all),
          "music repeat-all state is represented");
  require(player.set_volume_level(4), "firmware +4 volume is represented");
  require(!player.set_volume_level(5), "out-of-range volume is rejected");
  require(player.set_full_information_visible(true),
          "music full-information overlay is represented");

  auto scan = player.scan(ScanDirection::forward);
  require(scan && player.state().playback_rate == 1.5,
          "first forward scan is x1.5");
  require(player.scan(ScanDirection::forward) &&
              player.state().playback_rate == 10.0,
          "second forward scan is x10");
  require(player.scan(ScanDirection::forward) &&
              player.state().playback_rate == 30.0,
          "third forward scan is x30");
  require(player.scan(ScanDirection::forward) &&
              player.state().playback_rate == 120.0,
          "fourth forward scan is x120");
  require(player.scan(ScanDirection::forward) &&
              player.state().playback_rate == 1.5,
          "forward scan wraps to x1.5");
  require(player.scan(ScanDirection::reverse) &&
              player.state().playback_rate == -1.5,
          "reverse scan begins at x1.5");
  require(player.slow(ScanDirection::reverse) &&
              player.state().playback_rate == -0.5,
          "reverse slow playback is represented");
  require(player.end_scan() && player.state().playback_rate == 1.0,
          "ending scan resumes normal playback");

  require(player.pause(), "playing music can pause");
  require(!player.pause(), "repeated pause is rejected");
  require(player.play(), "paused music can resume");
  require(player.stop(), "music can stop");
  require_equal(player.state().position, 0ns, "stop rewinds music");
  require(player.unload(), "music can unload");
}

void test_video_repeat_and_control_state() {
  PlayerStateMachine player;
  require(player.load(video_request()), "representative video loads");
  require(player.set_screen_mode(ScreenMode::zoom),
          "video screen mode can change");
  require(player.set_zoom_pan({.zoom = 2.0, .pan_x = 0.25, .pan_y = -0.5}),
          "video zoom and pan are represented");
  require(player.set_panel(PlayerPanel::control_panel),
          "video control panel can open");
  require(player.set_submenu(PlayerSubmenu::audio),
          "video control-panel submenu can open");
  require_equal(player.state().focus, FocusTarget::submenu,
                "submenu owns focus while open");
  require(player.select_audio("audio.ja"),
          "alternate audio selection is represented");
  require(player.select_subtitle(std::nullopt),
          "subtitle Off state is represented");
  require(!player.select_subtitle(MediaId("subtitle.missing")),
          "unknown subtitle selection is rejected");
  require(player.set_audio_channel(AudioChannelMode::left),
          "dual-mono left channel state is represented");
  require(player.set_enhancement(Enhancement::block_noise_reduction, true),
          "block-noise reduction is represented");
  require(player.set_enhancement(Enhancement::upscale, true),
          "upscale enhancement is represented");
  require(player.set_osd_visible(true), "video OSD state is represented");

  require(player.set_repeat(RepeatMode::a_b), "A-B repeat can be selected");
  require(player.seek(10s), "seek to point A succeeds");
  require(player.mark_ab_point(), "point A can be marked");
  require(player.seek(10200ms), "seek near point A succeeds");
  const auto short_section = player.mark_ab_point();
  require(!short_section, "A-B section shorter than 500 ms is rejected");
  require_equal(short_section.error().code, MediaErrorCode::out_of_range,
                "short A-B section returns a typed range error");
  require(player.seek(20s), "seek to point B succeeds");
  require(player.mark_ab_point(), "point B can be marked");

  auto wrap = player.update_position(20s);
  require(wrap && wrap.value().effect == PlayerEffect::decoder_seek,
          "crossing point B requests a decoder seek");
  require_equal(player.state().position, 10s,
                "A-B repeat wraps exactly to point A");
  require(player.mark_ab_point(),
          "third A-B selection clears the progressive repeat state");
  require_equal(player.state().repeat, RepeatMode::off,
                "third A-B selection returns repeat to Off");

  require(player.set_ab_points(5s, 15s), "A-B points can be set directly");
  require(!player.set_ab_points(5s, 5200ms),
          "direct short A-B section is rejected");
  require(player.clear_ab_repeat(), "A-B repeat can be cleared");

  require(player.set_repeat(RepeatMode::title),
          "video title-repeat mode is represented");
  auto ended = player.update_position(100s);
  require(ended && ended.value().effect == PlayerEffect::decoder_seek,
          "title repeat requests a decoder seek");
  require_equal(player.state().position, 0ns,
                "title repeat wraps to the beginning");
}

void test_buffering_and_recovery_states() {
  PlayerStateMachine player;
  require(player.load(video_request()), "video loads for recovery test");
  require(player.begin_buffering("decoder is refilling queues"),
          "buffering can begin");
  require_equal(player.state().status, PlayerStatus::buffering,
                "buffering has an explicit player status");
  require(player.update_buffering(0.5),
          "buffering progress accepts normalized values");
  require(!player.update_buffering(1.5),
          "invalid buffering progress is rejected");
  require(player.finish_buffering(), "buffering can finish");
  require_equal(player.state().status, PlayerStatus::playing,
                "buffering restores the prior playback state");

  require(player.report_issue(make_media_error(MediaErrorCode::backend_failure,
                                               "decode",
                                               "hardware decode unavailable"),
                              IssueSeverity::degraded, true),
          "recoverable degradation is represented");
  require_equal(player.state().status, PlayerStatus::playing,
                "degraded mode keeps playback alive");
  require(player.clear_issue(), "degraded issue can be cleared");

  require(
      player.report_issue(make_media_error(MediaErrorCode::backend_failure,
                                           "decode", "decoder device was lost"),
                          IssueSeverity::fatal, true),
      "fatal decoder issue is represented");
  require_equal(player.state().status, PlayerStatus::error,
                "fatal issue moves player into explicit error state");
  require(!player.play(), "play cannot silently ignore a fatal issue");
  auto recovered = player.clear_issue();
  require(recovered &&
              recovered.value().effect == PlayerEffect::recover_backend,
          "clearing a fatal issue requests backend recovery");
  require_equal(
      player.state().status, PlayerStatus::paused,
      "recovery returns formerly playing media in a safe paused state");

  require(player.report_issue(
              make_media_error(MediaErrorCode::backend_failure, "decode",
                               "unrecoverable stream corruption"),
              IssueSeverity::fatal, false),
          "unrecoverable fatal issue is represented");
  require(!player.clear_issue(),
          "unrecoverable issue cannot be silently dismissed");
}

void test_invalid_player_load_inputs() {
  PlayerStateMachine player;
  auto duplicate_tracks = video_request();
  duplicate_tracks.subtitle_ids.push_back("subtitle.en");
  const auto duplicate = player.load(std::move(duplicate_tracks));
  require(!duplicate, "duplicate stream identifiers reject the load");
  require_equal(duplicate.error().code, MediaErrorCode::duplicate_id,
                "duplicate stream load returns a typed error");

  auto bad_default = video_request();
  bad_default.default_audio_id = "audio.missing";
  const auto missing = player.load(std::move(bad_default));
  require(!missing, "unknown default audio rejects the load");
  require_equal(missing.error().code, MediaErrorCode::missing_reference,
                "unknown default audio returns a typed error");
}

} // namespace

int main() {
  try {
    test_media_time_conversion();
    test_monotonic_playback_clock();
    test_audio_master_clock_and_discontinuities();
    test_catalog_validation_and_lookup();
    test_malformed_catalog_inputs();
    test_music_player_states_and_scanning();
    test_video_repeat_and_control_state();
    test_buffering_and_recovery_states();
    test_invalid_player_load_inputs();
    std::cout << "OpenXMB media foundation tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "OpenXMB media foundation test failure: " << error.what()
              << '\n';
    return 1;
  }
}
