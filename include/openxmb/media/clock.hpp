#pragma once

#include "openxmb/media/error.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace openxmb::media {

using MediaTime = std::chrono::nanoseconds;

inline constexpr MediaTime kDefaultDiscontinuityThreshold =
    std::chrono::milliseconds(250);
inline constexpr double kMinimumPlaybackRate = 0.125;
inline constexpr double kMaximumPlaybackRate = 16.0;

[[nodiscard]] MediaResult<MediaTime> media_time_from_seconds(double seconds);
[[nodiscard]] double media_time_seconds(MediaTime time) noexcept;

// A backend-independent monotonic source. Tests and deterministic capture tools
// provide a manual implementation; the production adapter can use
// SteadyMonotonicClock without exposing an operating-system clock type.
class IMonotonicClock {
public:
  virtual ~IMonotonicClock() = default;
  [[nodiscard]] virtual MediaTime now() const noexcept = 0;
};

class SteadyMonotonicClock final : public IMonotonicClock {
public:
  SteadyMonotonicClock() noexcept;
  [[nodiscard]] MediaTime now() const noexcept override;

private:
  std::chrono::steady_clock::time_point epoch_;
};

// The FFmpeg/audio backend implements only this seam. `sequence` must change
// after a decoder flush, seek, device reset, or other timeline discontinuity.
struct AudioClockReading {
  MediaTime position{};
  std::uint64_t sequence{};
  bool running{false};
};

class IAudioClock {
public:
  virtual ~IAudioClock() = default;
  [[nodiscard]] virtual MediaResult<AudioClockReading> read() const = 0;
};

enum class ClockMaster {
  audio,
  monotonic,
  // A seek has been issued but the audio backend has not yet acknowledged its
  // new sequence. This explicit transient prevents a stale audio timestamp from
  // undoing the seek.
  monotonic_pending_audio,
};

struct ClockConfiguration {
  MediaTime initial_position{};
  double playback_rate{1.0};
  bool audio_present{false};
  bool start_playing{false};
  MediaTime discontinuity_threshold{kDefaultDiscontinuityThreshold};
};

struct ClockSnapshot {
  MediaTime position{};
  ClockMaster master{ClockMaster::monotonic};
  double playback_rate{1.0};
  bool playing{false};
  bool audio_running{false};
  bool discontinuity{false};
  MediaTime audio_drift{};
  std::uint64_t discontinuity_generation{};
  std::optional<std::uint64_t> audio_sequence;
};

class PlaybackClock {
public:
  explicit PlaybackClock(IMonotonicClock &monotonic,
                         IAudioClock *audio = nullptr) noexcept;

  [[nodiscard]] MediaResult<ClockSnapshot>
  configure(const ClockConfiguration &configuration);
  [[nodiscard]] MediaResult<ClockSnapshot> sample();
  [[nodiscard]] MediaResult<ClockSnapshot> pause();
  [[nodiscard]] MediaResult<ClockSnapshot> resume();
  [[nodiscard]] MediaResult<ClockSnapshot> seek(MediaTime position);
  [[nodiscard]] MediaResult<ClockSnapshot> set_rate(double playback_rate);
  [[nodiscard]] MediaResult<ClockSnapshot> set_audio_present(bool present);

  [[nodiscard]] bool configured() const noexcept { return configured_; }
  [[nodiscard]] bool playing() const noexcept { return playing_; }
  [[nodiscard]] double rate() const noexcept { return rate_; }
  [[nodiscard]] bool audio_present() const noexcept { return audio_present_; }

private:
  [[nodiscard]] MediaResult<MediaTime> read_monotonic(const char *operation);
  [[nodiscard]] MediaResult<MediaTime>
  projected_position(MediaTime now, const char *operation) const;
  [[nodiscard]] MediaResult<ClockSnapshot> sample_at(MediaTime now,
                                                     const char *operation);
  [[nodiscard]] ClockSnapshot
  make_monotonic_snapshot(MediaTime position, ClockMaster master,
                          bool discontinuity = false,
                          MediaTime audio_drift = {}) const noexcept;

  IMonotonicClock &monotonic_;
  IAudioClock *audio_{};
  MediaTime anchor_position_{};
  MediaTime anchor_monotonic_{};
  MediaTime last_monotonic_{};
  MediaTime discontinuity_threshold_{kDefaultDiscontinuityThreshold};
  double rate_{1.0};
  bool configured_{false};
  bool playing_{false};
  bool audio_present_{false};
  bool have_monotonic_sample_{false};
  bool awaiting_audio_discontinuity_{false};
  std::optional<std::uint64_t> last_audio_sequence_;
  std::uint64_t discontinuity_generation_{};
};

} // namespace openxmb::media
