#pragma once
#include "openxmb/desktop/trophy_watch.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace openxmb::xmb {
// Monitor fractions of OpenXMB's existing in-shell trophy banner. This is a
// functional compact notice, not calibrated PS3 popup geometry: no original
// popup reference is available.
struct TrophyNoticeFrame { float x{},y{},width{},height{}; };
inline constexpr TrophyNoticeFrame trophy_notice_frame{0.66F,0.13F,0.31F,0.09F};
struct TrophyNoticeBounds {
  int x{},y{};
  unsigned width{},height{};
  float scale{}; // 1920x1080 design typography to this monitor
  bool operator==(const TrophyNoticeBounds&) const=default;
};
// Physical-pixel host rectangle for a monitor's virtual-desktop rectangle.
[[nodiscard]] inline std::optional<TrophyNoticeBounds> trophy_notice_bounds(int x,int y,unsigned width,unsigned height) {
  if(width<64||height<64||width>16384||height>16384)return std::nullopt;
  const auto left=static_cast<long long>(x)+std::lround(width*static_cast<double>(trophy_notice_frame.x));
  const auto top=static_cast<long long>(y)+std::lround(height*static_cast<double>(trophy_notice_frame.y));
  if(left<-2147483647LL||left>2147483647LL||top<-2147483647LL||top>2147483647LL)return std::nullopt;
  const auto w=std::max(1L,std::lround(width*static_cast<double>(trophy_notice_frame.width)));
  const auto h=std::max(1L,std::lround(height*static_cast<double>(trophy_notice_frame.height)));
  return TrophyNoticeBounds{static_cast<int>(left),static_cast<int>(top),static_cast<unsigned>(w),static_cast<unsigned>(h),
    std::min(static_cast<float>(width)/1920.0F,static_cast<float>(height)/1080.0F)};
}
// Bounded FIFO of value-only unlock edges. Only the front notice is timed; its
// hold starts at its first visible frame so preparation never consumes it.
class TrophyNoticeQueue {
public:
  static constexpr std::size_t capacity=16;
  static constexpr double hold_seconds=5.0,fade_in_seconds=0.18,fade_out_seconds=0.25;
  struct Entry { std::uint64_t serial{}; desktop::TrophyUnlockEvent event; };
  // Accepts only known-unlocked RPCS3 edges from origin. A full queue rejects
  // newer edges instead of discarding one already queued.
  bool push(desktop::TrophyUnlockEvent event,const desktop::TrophyWatchSession& origin) {
    if(!(event.session==origin)||event.provider!="rpcs3"||!event.achievement.unlock_state_known||!event.achievement.unlocked||entries_.size()>=capacity)
      return false;
    entries_.push_back({++serial_,std::move(event)});return true;
  }
  void clear() noexcept {entries_.clear();started_.reset();}
  [[nodiscard]] const Entry* current() const noexcept {return entries_.empty()?nullptr:&entries_.front();}
  [[nodiscard]] std::size_t size() const noexcept {return entries_.size();}
  [[nodiscard]] bool started() const noexcept {return started_.has_value();}
  void start(double now) noexcept {if(!entries_.empty()&&!started_&&std::isfinite(now))started_=now;}
  [[nodiscard]] float opacity(double now,bool reduced) const noexcept {
    if(entries_.empty()||!started_||!std::isfinite(now))return 0.0F;
    const double age=std::max(0.0,now-*started_);
    if(age>=hold_seconds)return 0.0F;
    if(reduced)return 1.0F;
    return static_cast<float>(std::clamp(std::min(age/fade_in_seconds,(hold_seconds-age)/fade_out_seconds),0.0,1.0));
  }
  // Retires the front notice after its hold. Returns whether one was retired.
  bool expire(double now) noexcept {
    if(entries_.empty()||!started_||!std::isfinite(now)||now-*started_<hold_seconds)return false;
    entries_.pop_front();started_.reset();return true;
  }
private:
  std::deque<Entry> entries_;
  std::optional<double> started_;
  std::uint64_t serial_{};
};
} // namespace openxmb::xmb
