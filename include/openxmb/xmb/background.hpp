#pragma once

#include <array>
#include <chrono>
#include <optional>
#include <string_view>

namespace openxmb::xmb {

struct BackgroundGradient {
  std::array<float, 3> top_rgb{};
  std::array<float, 3> bottom_rgb{};
  float night_day_blend{};
};

// Matches xmb-web's calibrated dawn/day/dusk/night windows.
[[nodiscard]] float night_day_blend(float local_hour) noexcept;

// month_zero_based is normalized into [0, 11]. Values are the measured
// xmb-web month anchors, including its 1.30 pre-compensation and July bottom
// override. Time of day is carried separately for the Vulkan vertical envelope.
[[nodiscard]] BackgroundGradient
resolve_background_gradient(int month_zero_based, float local_hour) noexcept;

// Manual PS3 Colour selections keep the same Original background structure and
// day/night envelope, but replace the month hue with the selected palette hue.
[[nodiscard]] BackgroundGradient
resolve_manual_background_gradient(std::array<float, 3> rgb,
                                   float local_hour) noexcept;

// Deterministic visual runs may pin wall time without changing normal runtime
// behavior. Invalid values are ignored and the live clock remains authoritative.
[[nodiscard]] std::optional<std::chrono::system_clock::time_point>
parse_unix_seconds(std::string_view value) noexcept;
[[nodiscard]] std::chrono::system_clock::time_point wall_clock_now() noexcept;

} // namespace openxmb::xmb
