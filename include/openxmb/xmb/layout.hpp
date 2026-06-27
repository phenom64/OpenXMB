#pragma once

#include <cstdint>

namespace openxmb::xmb {

inline constexpr double kLogicalWidth = 1920.0;
inline constexpr double kLogicalHeight = 1080.0;
inline constexpr double kLogicalAspectRatio = kLogicalWidth / kLogicalHeight;

struct Point {
  double x{};
  double y{};

  [[nodiscard]] constexpr bool
  operator==(const Point &) const noexcept = default;
};

struct Extent {
  double width{};
  double height{};

  [[nodiscard]] constexpr bool
  operator==(const Extent &) const noexcept = default;
};

struct Rect {
  Point origin{};
  Extent extent{};

  [[nodiscard]] constexpr Point center() const noexcept {
    return {origin.x + extent.width * 0.5, origin.y + extent.height * 0.5};
  }

  [[nodiscard]] constexpr bool
  operator==(const Rect &) const noexcept = default;
};

struct PixelExtent {
  std::uint32_t width{};
  std::uint32_t height{};

  [[nodiscard]] constexpr bool
  operator==(const PixelExtent &) const noexcept = default;
};

// `contain` is the OpenXMB presentation policy: preserve every audited 16:9
// anchor and letterbox/pillarbox the remaining framebuffer. The other modes
// are explicit tools for backdrop/media passes; UI code should not silently
// switch away from contain.
enum class LayoutScaleMode {
  contain,
  cover,
  stretch,
};

struct LayoutTransform {
  PixelExtent framebuffer{};
  LayoutScaleMode mode{LayoutScaleMode::contain};
  double scale_x{1.0};
  double scale_y{1.0};
  double offset_x{};
  double offset_y{};

  [[nodiscard]] Point point_to_framebuffer(Point logical) const noexcept;
  [[nodiscard]] Extent extent_to_framebuffer(Extent logical) const noexcept;
  [[nodiscard]] Rect rect_to_framebuffer(Rect logical) const noexcept;
  [[nodiscard]] Point point_to_logical(Point framebuffer_point) const noexcept;
  [[nodiscard]] Rect logical_viewport_in_framebuffer() const noexcept;
};

// Throws std::invalid_argument when either framebuffer dimension is zero.
[[nodiscard]] LayoutTransform
make_layout_transform(PixelExtent framebuffer,
                      LayoutScaleMode mode = LayoutScaleMode::contain);

} // namespace openxmb::xmb
