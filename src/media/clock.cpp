#include "openxmb/media/clock.hpp"

#include <cmath>
#include <limits>
#include <string>

namespace openxmb::media {
namespace {

[[nodiscard]] bool valid_rate(double rate) noexcept {
  return std::isfinite(rate) && rate >= kMinimumPlaybackRate &&
         rate <= kMaximumPlaybackRate;
}

[[nodiscard]] MediaTime absolute_time(MediaTime value) noexcept {
  if (value.count() >= 0) {
    return value;
  }
  if (value == MediaTime::min()) {
    return MediaTime::max();
  }
  return MediaTime(-value.count());
}

[[nodiscard]] MediaResult<MediaTime> scale_time(MediaTime elapsed, double rate,
                                                const char *operation) {
  const auto scaled = static_cast<long double>(elapsed.count()) *
                      static_cast<long double>(rate);
  const auto maximum =
      static_cast<long double>(std::numeric_limits<MediaTime::rep>::max());
  if (!std::isfinite(scaled) || scaled < 0.0L || scaled > maximum) {
    return MediaResult<MediaTime>::failure(make_media_error(
        MediaErrorCode::out_of_range, operation,
        "the projected media timestamp cannot be represented"));
  }
  return MediaResult<MediaTime>::success(
      MediaTime(static_cast<MediaTime::rep>(std::llround(scaled))));
}

} // namespace

MediaResult<MediaTime> media_time_from_seconds(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0.0) {
    return MediaResult<MediaTime>::failure(make_media_error(
        MediaErrorCode::invalid_argument, "media_time_from_seconds",
        "seconds must be finite and non-negative"));
  }

  const auto nanoseconds = static_cast<long double>(seconds) * 1'000'000'000.0L;
  const auto maximum =
      static_cast<long double>(std::numeric_limits<MediaTime::rep>::max());
  if (nanoseconds > maximum) {
    return MediaResult<MediaTime>::failure(make_media_error(
        MediaErrorCode::out_of_range, "media_time_from_seconds",
        "seconds exceed the representable media timeline"));
  }
  return MediaResult<MediaTime>::success(
      MediaTime(static_cast<MediaTime::rep>(std::llround(nanoseconds))));
}

double media_time_seconds(MediaTime time) noexcept {
  return std::chrono::duration<double>(time).count();
}

SteadyMonotonicClock::SteadyMonotonicClock() noexcept
    : epoch_(std::chrono::steady_clock::now()) {}

MediaTime SteadyMonotonicClock::now() const noexcept {
  return std::chrono::duration_cast<MediaTime>(
      std::chrono::steady_clock::now() - epoch_);
}

PlaybackClock::PlaybackClock(IMonotonicClock &monotonic,
                             IAudioClock *audio) noexcept
    : monotonic_(monotonic), audio_(audio) {}

MediaResult<MediaTime> PlaybackClock::read_monotonic(const char *operation) {
  const auto now = monotonic_.now();
  if (now.count() < 0) {
    return MediaResult<MediaTime>::failure(
        make_media_error(MediaErrorCode::clock_discontinuity, operation,
                         "the monotonic source returned a negative timestamp"));
  }
  if (have_monotonic_sample_ && now < last_monotonic_) {
    return MediaResult<MediaTime>::failure(
        make_media_error(MediaErrorCode::clock_discontinuity, operation,
                         "the monotonic source moved backwards"));
  }
  last_monotonic_ = now;
  have_monotonic_sample_ = true;
  return MediaResult<MediaTime>::success(now);
}

MediaResult<MediaTime>
PlaybackClock::projected_position(MediaTime now, const char *operation) const {
  if (!playing_) {
    return MediaResult<MediaTime>::success(anchor_position_);
  }
  if (now < anchor_monotonic_) {
    return MediaResult<MediaTime>::failure(
        make_media_error(MediaErrorCode::clock_discontinuity, operation,
                         "the sample precedes the playback anchor"));
  }
  auto scaled = scale_time(now - anchor_monotonic_, rate_, operation);
  if (!scaled) {
    return scaled;
  }
  if (scaled.value() > MediaTime::max() - anchor_position_) {
    return MediaResult<MediaTime>::failure(
        make_media_error(MediaErrorCode::out_of_range, operation,
                         "the projected media position overflowed"));
  }
  return MediaResult<MediaTime>::success(anchor_position_ + scaled.value());
}

ClockSnapshot
PlaybackClock::make_monotonic_snapshot(MediaTime position, ClockMaster master,
                                       bool discontinuity,
                                       MediaTime audio_drift) const noexcept {
  return {.position = position,
          .master = master,
          .playback_rate = rate_,
          .playing = playing_,
          .audio_running = false,
          .discontinuity = discontinuity,
          .audio_drift = audio_drift,
          .discontinuity_generation = discontinuity_generation_,
          .audio_sequence = last_audio_sequence_};
}

MediaResult<ClockSnapshot> PlaybackClock::sample_at(MediaTime now,
                                                    const char *operation) {
  if (!configured_) {
    return MediaResult<ClockSnapshot>::failure(
        make_media_error(MediaErrorCode::invalid_state, operation,
                         "the playback clock has not been configured"));
  }

  auto projected = projected_position(now, operation);
  if (!projected) {
    return MediaResult<ClockSnapshot>::failure(std::move(projected.error()));
  }

  if (!audio_present_) {
    return MediaResult<ClockSnapshot>::success(
        make_monotonic_snapshot(projected.value(), ClockMaster::monotonic));
  }

  if (!audio_) {
    return MediaResult<ClockSnapshot>::failure(make_media_error(
        MediaErrorCode::backend_unavailable, operation,
        "audio is present but no audio clock service is installed"));
  }

  // While paused the stable anchor is authoritative. The audio backend is not
  // required to manufacture advancing timestamps for a paused decoder.
  if (!playing_) {
    auto paused = make_monotonic_snapshot(anchor_position_, ClockMaster::audio);
    paused.audio_sequence = last_audio_sequence_;
    return MediaResult<ClockSnapshot>::success(std::move(paused));
  }

  auto audio_reading = audio_->read();
  if (!audio_reading) {
    auto error = std::move(audio_reading.error());
    if (error.operation.empty()) {
      error.operation = operation;
    }
    return MediaResult<ClockSnapshot>::failure(std::move(error));
  }
  const auto reading = audio_reading.value();
  if (reading.position.count() < 0) {
    return MediaResult<ClockSnapshot>::failure(
        make_media_error(MediaErrorCode::backend_failure, operation,
                         "the audio clock returned a negative media position"));
  }

  const auto drift = reading.position - projected.value();
  const bool stale_known_sequence =
      last_audio_sequence_ && reading.sequence == *last_audio_sequence_;
  const bool unsynchronised_first_sequence =
      !last_audio_sequence_ && absolute_time(drift) > discontinuity_threshold_;
  if (awaiting_audio_discontinuity_ &&
      (stale_known_sequence || unsynchronised_first_sequence)) {
    auto pending = make_monotonic_snapshot(
        projected.value(), ClockMaster::monotonic_pending_audio, false, drift);
    pending.audio_sequence = reading.sequence;
    return MediaResult<ClockSnapshot>::success(std::move(pending));
  }

  const bool acknowledged_seek = awaiting_audio_discontinuity_;
  awaiting_audio_discontinuity_ = false;
  const bool sequence_changed =
      last_audio_sequence_ && reading.sequence != *last_audio_sequence_;
  const bool drifted =
      last_audio_sequence_ && absolute_time(drift) > discontinuity_threshold_;
  const bool discontinuity = acknowledged_seek || sequence_changed || drifted;
  if ((sequence_changed || drifted) && !acknowledged_seek) {
    ++discontinuity_generation_;
  }

  anchor_position_ = reading.position;
  anchor_monotonic_ = now;
  last_audio_sequence_ = reading.sequence;

  ClockSnapshot snapshot{
      .position = reading.position,
      .master = ClockMaster::audio,
      .playback_rate = rate_,
      .playing = playing_,
      .audio_running = reading.running,
      .discontinuity = discontinuity,
      .audio_drift = drift,
      .discontinuity_generation = discontinuity_generation_,
      .audio_sequence = reading.sequence,
  };
  return MediaResult<ClockSnapshot>::success(std::move(snapshot));
}

MediaResult<ClockSnapshot>
PlaybackClock::configure(const ClockConfiguration &configuration) {
  if (configuration.initial_position.count() < 0) {
    return MediaResult<ClockSnapshot>::failure(
        make_media_error(MediaErrorCode::invalid_argument, "configure_clock",
                         "initial position must be non-negative"));
  }
  if (!valid_rate(configuration.playback_rate)) {
    return MediaResult<ClockSnapshot>::failure(make_media_error(
        MediaErrorCode::out_of_range, "configure_clock",
        "playback rate must be finite and within the supported clock range"));
  }
  if (configuration.discontinuity_threshold.count() <= 0) {
    return MediaResult<ClockSnapshot>::failure(
        make_media_error(MediaErrorCode::invalid_argument, "configure_clock",
                         "discontinuity threshold must be positive"));
  }
  if (configuration.audio_present && !audio_) {
    return MediaResult<ClockSnapshot>::failure(make_media_error(
        MediaErrorCode::backend_unavailable, "configure_clock",
        "audio media requires an installed audio clock service"));
  }

  configured_ = false;
  have_monotonic_sample_ = false;
  auto now = read_monotonic("configure_clock");
  if (!now) {
    return MediaResult<ClockSnapshot>::failure(std::move(now.error()));
  }

  anchor_position_ = configuration.initial_position;
  anchor_monotonic_ = now.value();
  discontinuity_threshold_ = configuration.discontinuity_threshold;
  rate_ = configuration.playback_rate;
  playing_ = configuration.start_playing;
  audio_present_ = configuration.audio_present;
  awaiting_audio_discontinuity_ = false;
  last_audio_sequence_.reset();
  discontinuity_generation_ = 0;
  configured_ = true;

  return sample_at(now.value(), "configure_clock");
}

MediaResult<ClockSnapshot> PlaybackClock::sample() {
  if (!configured_) {
    return MediaResult<ClockSnapshot>::failure(
        make_media_error(MediaErrorCode::invalid_state, "sample_clock",
                         "the playback clock has not been configured"));
  }
  auto now = read_monotonic("sample_clock");
  if (!now) {
    return MediaResult<ClockSnapshot>::failure(std::move(now.error()));
  }
  return sample_at(now.value(), "sample_clock");
}

MediaResult<ClockSnapshot> PlaybackClock::pause() {
  if (!configured_) {
    return MediaResult<ClockSnapshot>::failure(
        make_media_error(MediaErrorCode::invalid_state, "pause_clock",
                         "the playback clock has not been configured"));
  }
  if (!playing_) {
    return MediaResult<ClockSnapshot>::failure(
        make_media_error(MediaErrorCode::invalid_state, "pause_clock",
                         "the playback clock is already paused"));
  }

  auto now = read_monotonic("pause_clock");
  if (!now) {
    return MediaResult<ClockSnapshot>::failure(std::move(now.error()));
  }
  auto current = sample_at(now.value(), "pause_clock");
  if (!current) {
    return current;
  }
  anchor_position_ = current.value().position;
  anchor_monotonic_ = now.value();
  playing_ = false;
  auto snapshot = current.value();
  snapshot.playing = false;
  snapshot.audio_running = false;
  return MediaResult<ClockSnapshot>::success(std::move(snapshot));
}

MediaResult<ClockSnapshot> PlaybackClock::resume() {
  if (!configured_) {
    return MediaResult<ClockSnapshot>::failure(
        make_media_error(MediaErrorCode::invalid_state, "resume_clock",
                         "the playback clock has not been configured"));
  }
  if (playing_) {
    return MediaResult<ClockSnapshot>::failure(
        make_media_error(MediaErrorCode::invalid_state, "resume_clock",
                         "the playback clock is already running"));
  }
  auto now = read_monotonic("resume_clock");
  if (!now) {
    return MediaResult<ClockSnapshot>::failure(std::move(now.error()));
  }
  anchor_monotonic_ = now.value();
  playing_ = true;
  return sample_at(now.value(), "resume_clock");
}

MediaResult<ClockSnapshot> PlaybackClock::seek(MediaTime position) {
  if (!configured_) {
    return MediaResult<ClockSnapshot>::failure(
        make_media_error(MediaErrorCode::invalid_state, "seek_clock",
                         "the playback clock has not been configured"));
  }
  if (position.count() < 0) {
    return MediaResult<ClockSnapshot>::failure(
        make_media_error(MediaErrorCode::out_of_range, "seek_clock",
                         "seek position must be non-negative"));
  }
  auto now = read_monotonic("seek_clock");
  if (!now) {
    return MediaResult<ClockSnapshot>::failure(std::move(now.error()));
  }
  anchor_position_ = position;
  anchor_monotonic_ = now.value();
  ++discontinuity_generation_;
  awaiting_audio_discontinuity_ = audio_present_;

  const auto master =
      awaiting_audio_discontinuity_
          ? ClockMaster::monotonic_pending_audio
          : (audio_present_ ? ClockMaster::audio : ClockMaster::monotonic);
  return MediaResult<ClockSnapshot>::success(
      make_monotonic_snapshot(position, master, true));
}

MediaResult<ClockSnapshot> PlaybackClock::set_rate(double playback_rate) {
  if (!configured_) {
    return MediaResult<ClockSnapshot>::failure(
        make_media_error(MediaErrorCode::invalid_state, "set_clock_rate",
                         "the playback clock has not been configured"));
  }
  if (!valid_rate(playback_rate)) {
    return MediaResult<ClockSnapshot>::failure(make_media_error(
        MediaErrorCode::out_of_range, "set_clock_rate",
        "playback rate must be finite and within the supported clock range"));
  }
  if (playback_rate == rate_) {
    return sample();
  }

  auto now = read_monotonic("set_clock_rate");
  if (!now) {
    return MediaResult<ClockSnapshot>::failure(std::move(now.error()));
  }
  auto current = sample_at(now.value(), "set_clock_rate");
  if (!current) {
    return current;
  }
  anchor_position_ = current.value().position;
  anchor_monotonic_ = now.value();
  rate_ = playback_rate;
  auto snapshot = current.value();
  snapshot.playback_rate = rate_;
  return MediaResult<ClockSnapshot>::success(std::move(snapshot));
}

MediaResult<ClockSnapshot> PlaybackClock::set_audio_present(bool present) {
  if (!configured_) {
    return MediaResult<ClockSnapshot>::failure(
        make_media_error(MediaErrorCode::invalid_state, "set_audio_present",
                         "the playback clock has not been configured"));
  }
  if (present == audio_present_) {
    return sample();
  }
  if (present && !audio_) {
    return MediaResult<ClockSnapshot>::failure(make_media_error(
        MediaErrorCode::backend_unavailable, "set_audio_present",
        "audio media requires an installed audio clock service"));
  }

  auto now = read_monotonic("set_audio_present");
  if (!now) {
    return MediaResult<ClockSnapshot>::failure(std::move(now.error()));
  }
  auto current = sample_at(now.value(), "set_audio_present");
  if (!current) {
    return current;
  }
  anchor_position_ = current.value().position;
  anchor_monotonic_ = now.value();
  audio_present_ = present;
  last_audio_sequence_.reset();
  awaiting_audio_discontinuity_ = false;

  if (present && playing_) {
    return sample_at(now.value(), "set_audio_present");
  }
  const auto master = present ? ClockMaster::audio : ClockMaster::monotonic;
  return MediaResult<ClockSnapshot>::success(
      make_monotonic_snapshot(anchor_position_, master));
}

} // namespace openxmb::media
