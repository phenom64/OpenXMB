#include "openxmb/xmb/background.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

[[nodiscard]] bool near(float actual, float expected,
                        float epsilon = 0.0001F) {
  return std::abs(actual - expected) <= epsilon;
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "background contract failed: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

} // namespace

int main() {
  using openxmb::xmb::night_day_blend;
  using openxmb::xmb::parse_unix_seconds;
  using openxmb::xmb::resolve_background_gradient;
  using openxmb::xmb::resolve_manual_background_gradient;

  require(near(night_day_blend(13.0F), 0.0F), "midday is day");
  require(near(night_day_blend(18.5F), 0.5F), "dusk midpoint");
  require(near(night_day_blend(2.0F), 1.0F), "overnight is night");
  require(near(night_day_blend(5.75F), 0.5F), "dawn midpoint");

  const auto june = resolve_background_gradient(5, 13.0F);
  require(near(june.top_rgb[0], 0.227573F), "June red anchor");
  require(near(june.top_rgb[1], 0.821600F), "June green anchor");
  require(near(june.top_rgb[2], 0.687950F), "June blue anchor");
  require(june.top_rgb == june.bottom_rgb, "June has no bottom override");
  require(near(june.night_day_blend, 0.0F), "June midday blend");

  const auto january = resolve_background_gradient(0, 13.0F);
  const auto may = resolve_background_gradient(4, 13.0F);
  const auto september = resolve_background_gradient(8, 13.0F);
  require(january.top_rgb != june.top_rgb, "Original is not fixed to June");
  require(may.top_rgb != june.top_rgb, "May keeps its purple xmb-web anchor");
  require(september.top_rgb != june.top_rgb,
          "September keeps its gold xmb-web anchor");

  const auto july = resolve_background_gradient(6, 22.0F);
  require(july.top_rgb != july.bottom_rgb, "July bottom override");
  require(near(july.night_day_blend, 1.0F), "July night blend");

  const auto manual =
      resolve_manual_background_gradient({1.25F, 0.88F, -0.10F}, 22.0F);
  require(near(manual.top_rgb[0], 1.0F), "manual red clamps high");
  require(near(manual.top_rgb[1], 0.88F), "manual green keeps palette");
  require(near(manual.top_rgb[2], 0.0F), "manual blue clamps low");
  require(manual.top_rgb == manual.bottom_rgb,
          "manual colour keeps Original structure without month hue");
  require(near(manual.night_day_blend, 1.0F),
          "manual colour keeps day/night envelope");

  const auto pinned = parse_unix_seconds("1781524800");
  require(pinned.has_value(), "valid fixed timestamp parses");
  require(std::chrono::duration_cast<std::chrono::seconds>(
              pinned->time_since_epoch())
              .count() == 1781524800,
          "fixed timestamp value");
  require(!parse_unix_seconds("1781524800garbage").has_value(),
          "trailing garbage is rejected");
  require(!parse_unix_seconds("").has_value(), "empty timestamp is rejected");
}
