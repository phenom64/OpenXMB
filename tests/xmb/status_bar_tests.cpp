#include "openxmb/xmb/status_bar.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <span>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "status bar contract failed: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

[[nodiscard]] bool near(float actual, float expected,
                        float epsilon = 0.0001F) noexcept {
  return std::abs(actual - expected) <= epsilon;
}

[[nodiscard]] std::string_view format(openxmb::xmb::StatusBarTime time,
                                      std::span<char> buffer) {
  const auto size = openxmb::xmb::format_status_bar_time(time, buffer);
  return {buffer.data(), size};
}

} // namespace

int main() {
  using openxmb::xmb::StatusBarMetrics;
  using openxmb::xmb::StatusBarTime;
  using openxmb::xmb::resolve_clock_hand_angles;
  using openxmb::xmb::status_bar_time_from_tm;
  using openxmb::xmb::valid_status_bar_time;

  std::array<char, 16> text{};
  require(format({15, 6, 13, 0, 0}, text) == "15/6 13:00",
          "fixed scene uses compact day/month and two-digit minutes");
  require(format({1, 2, 3, 4, 5}, text) == "1/2 3:04",
          "day, month, and hour have no leading zero");
  require(format({31, 12, 23, 59, 60}, text) == "31/12 23:59",
          "maximum display fields fit the fixed stack buffer");

  std::array<char, 11> short_buffer{};
  require(openxmb::xmb::format_status_bar_time({31, 12, 23, 59, 0},
                                               short_buffer) == 0,
          "undersized output is rejected without truncation");
  require(short_buffer.front() == '\0',
          "failed formatting leaves an empty C string");
  require(!valid_status_bar_time({0, 6, 13, 0, 0}),
          "invalid civil fields are rejected");
  require(openxmb::xmb::format_status_bar_time({15, 13, 13, 0, 0}, text) ==
              0,
          "invalid values never reach rendering text");

  std::tm civil{};
  civil.tm_mday = 15;
  civil.tm_mon = 5;
  civil.tm_hour = 13;
  civil.tm_min = 0;
  civil.tm_sec = 7;
  require(status_bar_time_from_tm(civil) == StatusBarTime{15, 6, 13, 0, 7},
          "standard tm conversion normalizes the zero-based month");

  constexpr auto pi = 3.14159265358979323846F;
  const auto noon = resolve_clock_hand_angles({15, 6, 12, 0, 0});
  require(near(noon.hour_radians, -0.5F * pi),
          "hour hand points up at noon");
  require(near(noon.minute_radians, -0.5F * pi),
          "minute hand points up on the hour");
  const auto half_past = resolve_clock_hand_angles({15, 6, 3, 30, 0});
  require(near(half_past.minute_radians, 0.5F * pi),
          "minute hand points down at half past");
  require(near(half_past.hour_radians, pi / 12.0F),
          "hour hand includes fractional-hour movement");

  require(near(StatusBarMetrics::frame_x, 1329.0F) &&
              near(StatusBarMetrics::frame_y, 75.0F) &&
              near(StatusBarMetrics::frame_width, 590.0F) &&
              near(StatusBarMetrics::frame_height, 57.0F),
          "authoritative frame measurements remain pinned");
  require(near(StatusBarMetrics::icon_center_x, 1789.5F) &&
              near(StatusBarMetrics::icon_center_y, 103.5F) &&
              near(StatusBarMetrics::icon_radius, 16.0F),
          "authoritative clock icon measurements remain pinned");
  require(near(StatusBarMetrics::text_right_x, 1755.5F),
          "text ends at the measured icon gap");
}
