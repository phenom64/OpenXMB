#pragma once

#include <algorithm>
#include <cstddef>

namespace openxmb::xmb {

struct BootTextMeasure {
  double width{};
  double height{};
};

struct BootTextPlacement {
  double x{};
  double y{};
};

struct BootWarningBlockLayout {
  double x{};
  double first_baseline_y{};
  double first_top_y{};
  double font_size{};
  double line_pitch{};
};

// xmb-web places the coldboot logo/footer on the right side of the 1920x1080
// frame: centre (0.734W, 0.532H), width 0.365W. OpenXMB replaces only the logo
// artwork with the preserved identity string, kept as one fitted line inside
// the same right edge and vertical centre.
inline constexpr double kBootIdentityRightX = 0.734 + (0.365 * 0.5);
inline constexpr double kBootIdentityCenterY = 0.532;
inline constexpr double kBootIdentityBaseSize = 0.082;
inline constexpr double kBootIdentityMaxWidth = 0.365;
inline constexpr double kBootIdentityLinePitchScale = 1.16;
inline constexpr double kBootNativeTextMeasureToVisualScale = 0.5;

inline constexpr double kBootWarningReferenceFontSize = 24.0 / 1080.0;
inline constexpr double kBootWarningFontSize = 56.0 / 1080.0;
inline constexpr double kBootWarningLinePitch = 34.0 / 1080.0;
inline constexpr double kBootWarningWrapWidth = 1280.0 / 1920.0;

[[nodiscard]] constexpr double fit_startup_identity_size(
    double requested_size, double measured_width,
    double max_width = kBootIdentityMaxWidth) noexcept {
  if (requested_size <= 0.0 || measured_width <= 0.0 || max_width <= 0.0) {
    return std::max(0.0, requested_size);
  }
  return measured_width > max_width
             ? requested_size * (max_width / measured_width)
             : requested_size;
}

[[nodiscard]] constexpr BootTextPlacement
place_startup_identity_text(BootTextMeasure measure) noexcept {
  return {
      .x = kBootIdentityRightX - measure.width,
      .y = kBootIdentityCenterY - measure.height * 0.5,
  };
}

template <typename WidthRange>
[[nodiscard]] constexpr BootWarningBlockLayout
place_boot_warning_block(const WidthRange &line_widths) noexcept {
  double max_width = 0.0;
  std::size_t line_count = 0;
  for (const double width : line_widths) {
    max_width = std::max(max_width, width);
    ++line_count;
  }
  const double count = static_cast<double>(line_count);
  const double first_baseline =
      (1.0 - count * kBootWarningLinePitch) * 0.5 + kBootWarningFontSize;
  return {
      .x = (1.0 - max_width) * 0.5,
      .first_baseline_y = first_baseline,
      .first_top_y = first_baseline - kBootWarningFontSize,
      .font_size = kBootWarningFontSize,
      .line_pitch = kBootWarningLinePitch,
  };
}

} // namespace openxmb::xmb
