#pragma once

#include <cstdint>

namespace openxmb::xmb {

struct BootMilestones {
  static constexpr double wave_emerge_seconds = 0.8;
  static constexpr double identity_in_seconds = 2.2;
  static constexpr double identity_full_seconds = 4.0;
  static constexpr double identity_fade_seconds = 5.8;
  static constexpr double identity_out_seconds = 6.8;
  static constexpr double warning_in_seconds = 7.665;
  static constexpr double warning_out_seconds = 12.53;
  static constexpr double ui_in_seconds = 15.9;
  static constexpr double complete_seconds = 16.9;
};

enum class BootEffect : std::uint8_t {
  none,
  play_startup_cue,
};

struct BootSample {
  double elapsed_seconds{};

  // Renderer-facing background/wave tracks.
  double wave_boot_time_seconds{};
  double wave_gain{};
  double wave_geometry_progress{};

  // Identity layer. The host draws kStartupIdentity using these values.
  double identity_opacity{};
  double identity_blur_px{};
  double identity_scale{1.0};

  // Warning text stays sharp; this blur value belongs to its backdrop.
  double warning_opacity{};
  double warning_backdrop_blur_px{};

  // Staged root-scene reveal.
  double ui_reveal{};
  double label_reveal{};
  double icon_reveal{};

  bool complete{};
  bool skipped{};
  BootEffect effect{BootEffect::none};
};

// Stateless and deterministic: useful for rendering, seekable captures, and
// milestone tests. `elapsed_seconds` is clamped to zero.
[[nodiscard]] BootSample sample_boot_timeline(double elapsed_seconds) noexcept;

// Stateful edge detector around the pure timeline. It emits the semantic NSE
// startup request exactly once and owns no audio backend.
class BootTimeline {
public:
  [[nodiscard]] BootSample sample(double elapsed_seconds) noexcept;
  [[nodiscard]] BootSample skip(double elapsed_seconds = 0.0) noexcept;
  void reset() noexcept;

  [[nodiscard]] bool skipped() const noexcept { return skipped_; }
  [[nodiscard]] bool startup_cue_emitted() const noexcept {
    return startup_cue_emitted_;
  }

private:
  bool skipped_{};
  bool startup_cue_emitted_{};
};

} // namespace openxmb::xmb
