#include "openxmb/xmb/status_bar.hpp"

#include <charconv>
#include <cmath>

namespace openxmb::xmb {
namespace {

[[nodiscard]] bool append_number(int value, char *&cursor,
                                 char *end) noexcept {
  const auto result = std::to_chars(cursor, end, value);
  if (result.ec != std::errc{}) {
    return false;
  }
  cursor = result.ptr;
  return true;
}

[[nodiscard]] bool append_character(char value, char *&cursor,
                                    char *end) noexcept {
  if (cursor == end) {
    return false;
  }
  *cursor++ = value;
  return true;
}

[[nodiscard]] bool append_two_digits(int value, char *&cursor,
                                     char *end) noexcept {
  return append_character(static_cast<char>('0' + value / 10), cursor, end) &&
         append_character(static_cast<char>('0' + value % 10), cursor, end);
}

} // namespace

std::size_t format_status_bar_time(const StatusBarTime &time,
                                   std::span<char> destination) noexcept {
  if (!valid_status_bar_time(time) || destination.empty()) {
    if (!destination.empty()) {
      destination.front() = '\0';
    }
    return 0;
  }

  auto *cursor = destination.data();
  // Reserve one byte throughout for the required terminator.
  auto *end = destination.data() + destination.size() - 1;
  const auto success = append_number(time.day, cursor, end) &&
                       append_character('/', cursor, end) &&
                       append_number(time.month, cursor, end) &&
                       append_character(' ', cursor, end) &&
                       append_number(time.hour, cursor, end) &&
                       append_character(':', cursor, end) &&
                       append_two_digits(time.minute, cursor, end);
  if (!success || cursor > end) {
    destination.front() = '\0';
    return 0;
  }
  *cursor = '\0';
  return static_cast<std::size_t>(cursor - destination.data());
}

StatusBarTime status_bar_time_from_tm(const std::tm &time) noexcept {
  return {
      .day = time.tm_mday,
      .month = time.tm_mon + 1,
      .hour = time.tm_hour,
      .minute = time.tm_min,
      .second = time.tm_sec,
  };
}

std::optional<StatusBarTime> resolve_local_status_bar_time(
    std::chrono::system_clock::time_point time) noexcept {
  const auto raw_time = std::chrono::system_clock::to_time_t(time);
  std::tm local{};
#if defined(_WIN32)
  if (::localtime_s(&local, &raw_time) != 0) {
    return std::nullopt;
  }
#else
  if (::localtime_r(&raw_time, &local) == nullptr) {
    return std::nullopt;
  }
#endif
  const auto result = status_bar_time_from_tm(local);
  return valid_status_bar_time(result) ? std::optional{result} : std::nullopt;
}

ClockHandAngles resolve_clock_hand_angles(const StatusBarTime &time) noexcept {
  if (!valid_status_bar_time(time)) {
    return {};
  }
  constexpr auto pi = 3.14159265358979323846F;
  constexpr auto full_turn = 2.0F * pi;
  constexpr auto twelve_oclock = -0.5F * pi;
  const auto minute = static_cast<float>(time.minute) +
                      static_cast<float>(time.second) / 60.0F;
  const auto hour = static_cast<float>(time.hour % 12) + minute / 60.0F;
  return {
      .hour_radians = twelve_oclock + hour / 12.0F * full_turn,
      .minute_radians = twelve_oclock + minute / 60.0F * full_turn,
  };
}

} // namespace openxmb::xmb
