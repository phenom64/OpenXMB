#include "openxmb/xmb/layout.hpp"

#include <algorithm>
#include <stdexcept>

namespace openxmb::xmb {

Point LayoutTransform::point_to_framebuffer(Point logical) const noexcept {
  return {
      offset_x + logical.x * scale_x,
      offset_y + logical.y * scale_y,
  };
}

Extent LayoutTransform::extent_to_framebuffer(Extent logical) const noexcept {
  return {logical.width * scale_x, logical.height * scale_y};
}

Rect LayoutTransform::rect_to_framebuffer(Rect logical) const noexcept {
  return {
      point_to_framebuffer(logical.origin),
      extent_to_framebuffer(logical.extent),
  };
}

Point LayoutTransform::point_to_logical(
    Point framebuffer_point) const noexcept {
  return {
      (framebuffer_point.x - offset_x) / scale_x,
      (framebuffer_point.y - offset_y) / scale_y,
  };
}

Rect LayoutTransform::logical_viewport_in_framebuffer() const noexcept {
  return {{offset_x, offset_y},
          {kLogicalWidth * scale_x, kLogicalHeight * scale_y}};
}

LayoutTransform make_layout_transform(PixelExtent framebuffer,
                                      LayoutScaleMode mode) {
  if (framebuffer.width == 0 || framebuffer.height == 0) {
    throw std::invalid_argument(
        "OpenXMB layout requires a non-zero framebuffer extent");
  }

  const auto width = static_cast<double>(framebuffer.width);
  const auto height = static_cast<double>(framebuffer.height);
  const auto width_scale = width / kLogicalWidth;
  const auto height_scale = height / kLogicalHeight;

  LayoutTransform result{
      .framebuffer = framebuffer,
      .mode = mode,
  };

  switch (mode) {
  case LayoutScaleMode::contain: {
    const auto scale = std::min(width_scale, height_scale);
    result.scale_x = scale;
    result.scale_y = scale;
    break;
  }
  case LayoutScaleMode::cover: {
    const auto scale = std::max(width_scale, height_scale);
    result.scale_x = scale;
    result.scale_y = scale;
    break;
  }
  case LayoutScaleMode::stretch:
    result.scale_x = width_scale;
    result.scale_y = height_scale;
    break;
  }

  result.offset_x = (width - kLogicalWidth * result.scale_x) * 0.5;
  result.offset_y = (height - kLogicalHeight * result.scale_y) * 0.5;
  return result;
}

} // namespace openxmb::xmb
