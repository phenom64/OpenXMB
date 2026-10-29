#pragma once
#include "openxmb/integrations/desktop.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace openxmb::integrations {
struct OverlayBounds { int x{}, y{}; unsigned width{}, height{}; };
enum class OverlayInputKind { key_down, key_up, close, focus_lost, mouse_move, mouse_down, mouse_up };
struct OverlayInput {
  OverlayInputKind kind{};
  unsigned virtual_key{}, scan_code{}, modifiers{};
  int x{}, y{};
  bool repeat{};
};
struct OverlayStats { std::uint64_t frames{}, conversion_ns{}, present_ns{}; };
using OverlayCallback=std::function<void(const OverlayInput&)>;
// Converts sRGB-encoded linear-premultiplied BGRA to 8-bit sRGB-premultiplied
// BGRA. Strides are bytes; input and output must not overlap.
[[nodiscard]] Result convert_overlay_bgra(std::span<const std::uint8_t> input, unsigned width, unsigned height,
  std::size_t input_stride, std::span<std::uint8_t> output, std::size_t output_stride);
// Native operations and callbacks belong to the creating thread. Callbacks must
// queue application work and must not destroy this object reentrantly.
class XmbOverlay {
public:
  XmbOverlay();
  ~XmbOverlay();
  XmbOverlay(const XmbOverlay&)=delete;
  XmbOverlay& operator=(const XmbOverlay&)=delete;
  [[nodiscard]] Result prepare(OverlayBounds bounds, OverlayCallback callback);
  // Input is consumed synchronously and never retained. Present a first frame
  // before show(); a failed activation hides the native window again.
  [[nodiscard]] Result present(std::span<const std::uint8_t> bgra, unsigned width, unsigned height, std::size_t stride);
  [[nodiscard]] Result show(bool activate=true);
  void hide();
  [[nodiscard]] bool visible() const;
  [[nodiscard]] bool focused() const;
  [[nodiscard]] std::uintptr_t native_handle() const;
  [[nodiscard]] OverlayStats stats() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace openxmb::integrations
