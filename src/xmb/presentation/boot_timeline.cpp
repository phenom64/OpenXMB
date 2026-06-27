#include "openxmb/xmb/boot_timeline.hpp"

#include <algorithm>
#include <cmath>

namespace openxmb::xmb {
namespace {

[[nodiscard]] constexpr double saturate(double value) noexcept {
  return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] constexpr double ramp(double time, double start,
                                    double end) noexcept {
  return end <= start ? (time >= end ? 1.0 : 0.0)
                      : saturate((time - start) / (end - start));
}

[[nodiscard]] constexpr double smooth(double value) noexcept {
  const auto t = saturate(value);
  return t * t * (3.0 - 2.0 * t);
}

} // namespace

BootSample sample_boot_timeline(double elapsed_seconds) noexcept {
  using M = BootMilestones;
  const auto time =
      std::isfinite(elapsed_seconds) ? std::max(0.0, elapsed_seconds) : 0.0;

  BootSample sample{};
  sample.elapsed_seconds = time;
  sample.wave_boot_time_seconds = std::max(0.0, time - M::wave_emerge_seconds);
  sample.wave_gain = smooth(ramp(time, M::wave_emerge_seconds, 2.6));
  sample.wave_geometry_progress = ramp(time, 1.3, 4.2);

  if (time >= M::identity_in_seconds && time < M::identity_full_seconds) {
    const auto progress =
        smooth(ramp(time, M::identity_in_seconds, M::identity_full_seconds));
    sample.identity_opacity = progress;
    sample.identity_blur_px = 6.5 * (1.0 - progress);
    sample.identity_scale = 0.985 + progress * 0.015;
  } else if (time >= M::identity_full_seconds &&
             time < M::identity_fade_seconds) {
    sample.identity_opacity = 1.0;
    sample.identity_blur_px = 0.0;
    sample.identity_scale = 1.0;
  } else if (time >= M::identity_fade_seconds &&
             time < M::identity_out_seconds) {
    const auto progress =
        smooth(ramp(time, M::identity_fade_seconds, M::identity_out_seconds));
    sample.identity_opacity = 1.0 - progress;
    sample.identity_blur_px = 6.5 * progress;
    sample.identity_scale = 1.0 + progress * 0.015;
  } else {
    sample.identity_scale = time < M::identity_in_seconds ? 0.985 : 1.015;
  }

  sample.warning_opacity =
      time >= M::warning_in_seconds && time < M::warning_out_seconds ? 1.0
                                                                     : 0.0;

  // Match xmb-web's audited warning-background blur bracket: finish the
  // 300 ms ramp 250 ms before the hard cut, hold 120 ms after dismissal,
  // then release over 220 ms.
  const auto blur_in = smooth(
      ramp(time, M::warning_in_seconds - 0.55, M::warning_in_seconds - 0.25));
  const auto blur_out = 1.0 - smooth(ramp(time, M::warning_out_seconds + 0.12,
                                          M::warning_out_seconds + 0.34));
  sample.warning_backdrop_blur_px = 6.5 * blur_in * blur_out;

  sample.ui_reveal = ramp(time, M::ui_in_seconds, M::complete_seconds);
  sample.label_reveal = smooth(
      ramp(time, M::ui_in_seconds,
           M::ui_in_seconds + 0.45 * (M::complete_seconds - M::ui_in_seconds)));
  sample.icon_reveal = smooth(ramp(
      time, M::ui_in_seconds + 0.10 * (M::complete_seconds - M::ui_in_seconds),
      M::complete_seconds));
  sample.complete = time >= M::complete_seconds;
  return sample;
}

BootSample BootTimeline::sample(double elapsed_seconds) noexcept {
  if (skipped_) {
    auto result = sample_boot_timeline(BootMilestones::complete_seconds);
    result.elapsed_seconds = std::max(0.0, elapsed_seconds);
    result.skipped = true;
    return result;
  }

  auto result = sample_boot_timeline(elapsed_seconds);
  if (!startup_cue_emitted_ && !result.complete) {
    result.effect = BootEffect::play_startup_cue;
    startup_cue_emitted_ = true;
  }
  return result;
}

BootSample BootTimeline::skip(double elapsed_seconds) noexcept {
  skipped_ = true;
  auto result = sample_boot_timeline(BootMilestones::complete_seconds);
  result.elapsed_seconds = std::max(0.0, elapsed_seconds);
  result.skipped = true;
  return result;
}

void BootTimeline::reset() noexcept {
  skipped_ = false;
  startup_cue_emitted_ = false;
}

} // namespace openxmb::xmb
