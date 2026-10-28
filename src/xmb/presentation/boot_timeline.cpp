#include "openxmb/xmb/boot_timeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace openxmb::xmb {
namespace {
template<std::size_t N> double track(double time,const std::array<std::array<double,2>,N>& keys) noexcept {
    if(time<=keys.front()[0]) return keys.front()[1];
    for(std::size_t i=1;i<N;++i) if(time<=keys[i][0]) {
        const double p=(time-keys[i-1][0])/(keys[i][0]-keys[i-1][0]);return keys[i-1][1]+p*(keys[i][1]-keys[i-1][1]);
    }
    return keys.back()[1];
}
constexpr std::array<std::array<double,2>,43> identity_left{{{0,0},{1.92,0},{1.99,0.04},{2.06,0.06},{2.12,0.1},{2.16,0.13},{2.19,0.19},{2.22,0.25},{2.26,0.31},{2.29,0.37},{2.32,0.48},{2.36,0.61},{2.39,0.72},{2.42,0.82},{2.46,0.9},{2.49,0.95},{2.56,0.98},{2.66,0.99},{3.0,1},{4.23,1},{4.29,0.98},{4.33,0.95},{4.36,0.92},{4.39,0.89},{4.43,0.84},{4.46,0.77},{4.49,0.67},{4.53,0.59},{4.56,0.52},{4.59,0.43},{4.63,0.33},{4.66,0.27},{4.69,0.23},{4.73,0.19},{4.76,0.15},{4.81,0.12},{4.86,0.09},{4.91,0.06},{4.96,0.05},{5.01,0.04},{5.06,0.02},{5.11,0.01},{5.16,0}}};
constexpr std::array<std::array<double,2>,39> identity_middle{{{0,0},{1.96,0},{1.99,0.02},{2.12,0.04},{2.16,0.06},{2.19,0.08},{2.22,0.11},{2.26,0.13},{2.29,0.16},{2.32,0.24},{2.36,0.31},{2.39,0.42},{2.42,0.55},{2.46,0.71},{2.49,0.86},{2.56,0.98},{2.66,0.98},{3.0,1},{4.26,1},{4.29,0.99},{4.33,0.96},{4.39,0.93},{4.43,0.89},{4.46,0.84},{4.49,0.76},{4.53,0.69},{4.56,0.63},{4.59,0.55},{4.63,0.45},{4.66,0.37},{4.69,0.32},{4.73,0.26},{4.76,0.21},{4.81,0.13},{4.86,0.08},{4.91,0.05},{4.96,0.04},{5.01,0.03},{5.16,0}}};
constexpr std::array<std::array<double,2>,45> identity_right{{{0,0},{2.09,0},{2.12,0.02},{2.19,0.03},{2.22,0.05},{2.29,0.06},{2.32,0.12},{2.36,0.16},{2.39,0.22},{2.42,0.29},{2.46,0.41},{2.49,0.57},{2.56,0.8},{2.59,0.95},{2.69,0.96},{3.0,1},{4.05,1},{4.09,0.97},{4.16,0.95},{4.23,0.93},{4.26,0.9},{4.29,0.86},{4.33,0.79},{4.36,0.74},{4.39,0.71},{4.43,0.64},{4.46,0.61},{4.49,0.55},{4.53,0.5},{4.56,0.48},{4.59,0.45},{4.63,0.4},{4.66,0.36},{4.69,0.34},{4.73,0.3},{4.76,0.27},{4.81,0.2},{4.86,0.15},{4.91,0.09},{4.96,0.07},{5.01,0.05},{5.06,0.03},{5.1,0.02},{5.36,0.02},{5.41,0}}};

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

// Integrate the velocity envelope, rather than multiplying time by a changing
// speed: that would jump the ribbon's phase when the intro settles.
// Measured on the firmware's boot (RPCS3): the wave already flows about twice
// as fast as normal when it emerges, peaks as the identity arrives, and is
// back to the regular speed by the warning.
[[nodiscard]] double startup_wave_time(double elapsed) noexcept {
  const double time = std::max(0.0, elapsed - BootMilestones::wave_emerge_seconds);
  constexpr double peak = 1.9, easing = 4.0;
  constexpr double settled = BootMilestones::warning_in_seconds -
                             BootMilestones::wave_emerge_seconds;
  // Distance covered while easing between two speeds over `length` seconds.
  const auto segment = [](double duration, double length, double from, double to) {
    const double t = std::clamp(duration / length, 0.0, 1.0);
    return length * (from * t + (to - from) * (t * t * t - 0.5 * t * t * t * t));
  };
  return segment(time, peak, 2.0, 3.2) +
         segment(time - peak, easing - peak, 3.2, 1.8) +
         segment(time - easing, settled - easing, 1.8, 1.0) +
         std::max(0.0, time - settled);
}

[[nodiscard]] double gradient_reveal(double milliseconds,
                                     std::size_t column) noexcept {
  // Display-space exposure measured on the firmware's boot (RPCS3), timed
  // to the identity: black until it arrives, then the top leads, overshoots
  // slightly and settles; the bottom follows.
  constexpr std::array table{
      std::array{2200.0, 0.00, 0.00}, std::array{2400.0, 0.04, 0.00},
      std::array{2800.0, 0.17, 0.10}, std::array{3200.0, 0.30, 0.21},
      std::array{3600.0, 0.44, 0.32}, std::array{4000.0, 0.58, 0.43},
      std::array{4400.0, 0.70, 0.52}, std::array{4800.0, 0.83, 0.62},
      std::array{5200.0, 0.94, 0.72}, std::array{5600.0, 1.02, 0.78},
      std::array{6000.0, 1.10, 0.86}, std::array{6400.0, 1.06, 0.91},
      std::array{6800.0, 1.00, 0.98}, std::array{7200.0, 1.00, 1.00},
  };
  if (milliseconds <= table.front()[0]) {
    return 0.0;
  }
  if (milliseconds >= table.back()[0]) {
    return 1.0;
  }
  for (std::size_t index = 0; index + 1 < table.size(); ++index) {
    const auto &from = table[index];
    const auto &to = table[index + 1];
    if (milliseconds >= from[0] && milliseconds <= to[0]) {
      const auto local = (milliseconds - from[0]) / (to[0] - from[0]);
      return from[column] + (to[column] - from[column]) * local;
    }
  }
  return 1.0;
}

} // namespace

BootSample sample_boot_timeline(double elapsed_seconds) noexcept {
  using M = BootMilestones;
  const auto time =
      std::isfinite(elapsed_seconds) ? std::max(0.0, elapsed_seconds) : 0.0;

  BootSample sample{};
  sample.elapsed_seconds = time;
  sample.wave_boot_time_seconds = startup_wave_time(time);
  sample.wave_gain = smooth(ramp(time, M::wave_emerge_seconds, 2.6));
  sample.wave_geometry_progress = ramp(time, 1.3, 4.2);
  sample.background_active = time < M::complete_seconds;
  const auto milliseconds = time * 1000.0;
  sample.background_exposure_top = gradient_reveal(milliseconds, 1);
  sample.background_exposure_bottom = gradient_reveal(milliseconds, 2);
  sample.customise =
      smooth(ramp(time, M::ui_in_seconds, M::customised_seconds));

  sample.identity_gradient={track(time,identity_left),track(time,identity_middle),track(time,identity_right)};
  sample.identity_opacity=*std::max_element(sample.identity_gradient.begin(),sample.identity_gradient.end());
  sample.identity_scale=1.0;
  sample.identity_blur_px=0.0;

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
    auto result = sample_boot_timeline(BootMilestones::customised_seconds);
    result.elapsed_seconds = std::max(0.0, elapsed_seconds);
    result.skipped = true;
    return result;
  }

  auto result = sample_boot_timeline(elapsed_seconds + warning_skip_seconds_);
  result.wave_boot_time_seconds = startup_wave_time(elapsed_seconds);
  if (!startup_cue_emitted_ && !result.complete) {
    result.effect = BootEffect::play_startup_cue;
    startup_cue_emitted_ = true;
  }
  return result;
}

bool BootTimeline::dismiss_warning(double elapsed_seconds) noexcept {
  const auto current = sample_boot_timeline(elapsed_seconds + warning_skip_seconds_);
  if (skipped_ || current.warning_opacity <= 0.0) {
    return false;
  }
  warning_skip_seconds_ += BootMilestones::ui_in_seconds - current.elapsed_seconds;
  return true;
}

BootSample BootTimeline::skip(double elapsed_seconds) noexcept {
  skipped_ = true;
  auto result = sample_boot_timeline(BootMilestones::customised_seconds);
  result.elapsed_seconds = std::max(0.0, elapsed_seconds);
  result.skipped = true;
  return result;
}

void BootTimeline::reset() noexcept {
  warning_skip_seconds_ = 0.0;
  skipped_ = false;
  startup_cue_emitted_ = false;
}

} // namespace openxmb::xmb
