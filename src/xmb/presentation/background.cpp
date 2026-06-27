#include "openxmb/xmb/background.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace openxmb::xmb {
namespace {

struct HsvAnchor {
  float hue_degrees;
  float saturation;
  float value;
};

constexpr std::array<HsvAnchor, 12> kMonthAnchors{{
    {58.6F, 0.630F, 0.785F},
    {88.9F, 0.650F, 0.593F},
    {339.8F, 0.562F, 0.708F},
    {118.1F, 0.713F, 0.277F},
    {270.7F, 0.475F, 0.440F},
    {166.5F, 0.723F, 0.632F},
    {196.0F, 0.620F, 0.585F},
    {288.0F, 0.600F, 0.672F},
    {47.6F, 0.705F, 0.816F},
    {33.2F, 0.590F, 0.314F},
    {0.9F, 0.668F, 0.621F},
    {0.0F, 0.000F, 0.808F},
}};

constexpr HsvAnchor kJulyBottom{242.0F, 0.900F, 0.230F};
constexpr float kValuePrecompensation = 1.30F;

[[nodiscard]] constexpr int wrap_month(int month) noexcept {
  const auto wrapped = month % 12;
  return wrapped < 0 ? wrapped + 12 : wrapped;
}

[[nodiscard]] std::array<float, 3> hsv_to_rgb(HsvAnchor anchor) noexcept {
  auto hue = std::fmod(anchor.hue_degrees, 360.0F);
  if (hue < 0.0F) {
    hue += 360.0F;
  }
  const auto saturation = std::clamp(anchor.saturation, 0.0F, 1.0F);
  const auto value = std::max(anchor.value * kValuePrecompensation, 0.0F);
  const auto chroma = value * saturation;
  const auto x = chroma * (1.0F - std::abs(std::fmod(hue / 60.0F, 2.0F) - 1.0F));
  const auto match = value - chroma;

  std::array<float, 3> rgb{};
  if (hue < 60.0F) {
    rgb = {chroma, x, 0.0F};
  } else if (hue < 120.0F) {
    rgb = {x, chroma, 0.0F};
  } else if (hue < 180.0F) {
    rgb = {0.0F, chroma, x};
  } else if (hue < 240.0F) {
    rgb = {0.0F, x, chroma};
  } else if (hue < 300.0F) {
    rgb = {x, 0.0F, chroma};
  } else {
    rgb = {chroma, 0.0F, x};
  }
  for (auto &channel : rgb) {
    channel += match;
  }
  return rgb;
}

[[nodiscard]] std::optional<std::chrono::system_clock::time_point>
fixed_time_from_environment() noexcept {
#if defined(_WIN32)
  char *buffer{};
  std::size_t size{};
  if (_dupenv_s(&buffer, &size, "OPENXMB_FIXED_UNIX_SECONDS") != 0 ||
      buffer == nullptr) {
    return std::nullopt;
  }
  const auto parsed = parse_unix_seconds(buffer);
  std::free(buffer);
  return parsed;
#else
  const auto *value = std::getenv("OPENXMB_FIXED_UNIX_SECONDS");
  return value == nullptr
             ? std::optional<std::chrono::system_clock::time_point>{}
             : parse_unix_seconds(value);
#endif
}

} // namespace

float night_day_blend(float local_hour) noexcept {
  if (!std::isfinite(local_hour)) {
    return 0.0F;
  }
  auto hour = std::fmod(local_hour, 24.0F);
  if (hour < 0.0F) {
    hour += 24.0F;
  }
  if (hour >= 16.5F && hour <= 20.5F) {
    return (hour - 16.5F) / 4.0F;
  }
  if (hour > 20.5F || hour < 4.5F) {
    return 1.0F;
  }
  if (hour >= 4.5F && hour <= 7.0F) {
    return 1.0F - (hour - 4.5F) / 2.5F;
  }
  return 0.0F;
}

BackgroundGradient resolve_background_gradient(int month_zero_based,
                                               float local_hour) noexcept {
  const auto month = wrap_month(month_zero_based);
  const auto top = hsv_to_rgb(kMonthAnchors[static_cast<std::size_t>(month)]);
  const auto bottom = month == 6 ? hsv_to_rgb(kJulyBottom) : top;
  return {
      .top_rgb = top,
      .bottom_rgb = bottom,
      .night_day_blend = night_day_blend(local_hour),
  };
}

std::optional<std::chrono::system_clock::time_point>
parse_unix_seconds(std::string_view value) noexcept {
  if (value.empty()) {
    return std::nullopt;
  }
  std::int64_t seconds{};
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), seconds);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  using seconds_duration = std::chrono::seconds;
  return std::chrono::system_clock::time_point{seconds_duration{seconds}};
}

std::chrono::system_clock::time_point wall_clock_now() noexcept {
  static const auto fixed_time = fixed_time_from_environment();
  return fixed_time.value_or(std::chrono::system_clock::now());
}

} // namespace openxmb::xmb
