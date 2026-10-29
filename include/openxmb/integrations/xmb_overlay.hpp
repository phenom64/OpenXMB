#pragma once
#include "openxmb/integrations/desktop.hpp"
#include "openxmb/integrations/session_window.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
// interactive: focusable host that routes keyboard/mouse/close/focus events.
// passive: topmost, never-activating, input-transparent notice host. It
// never invokes a callback, never takes focus and fails where the platform
// cannot make it input-transparent.
enum class OverlayInteraction { interactive, passive };
// Converts sRGB-encoded linear-premultiplied BGRA to 8-bit sRGB-premultiplied
// BGRA. Strides are bytes; input and output must not overlap.
[[nodiscard]] Result convert_overlay_bgra(std::span<const std::uint8_t> input, unsigned width, unsigned height,
  std::size_t input_stride, std::span<std::uint8_t> output, std::size_t output_stride);
// Native operations and callbacks belong to the creating thread. Callbacks must
// queue application work and must not destroy this object reentrantly.
// Failures of native compositor calls carry the platform error code.
class XmbOverlay {
public:
  XmbOverlay();
  ~XmbOverlay();
  XmbOverlay(const XmbOverlay&)=delete;
  XmbOverlay& operator=(const XmbOverlay&)=delete;
  [[nodiscard]] Result prepare(OverlayBounds bounds, OverlayCallback callback,
    OverlayInteraction interaction=OverlayInteraction::interactive);
  // Moves and resizes a prepared host without recreating its native window,
  // so visibility, stacking and focus survive. Every later frame must use the
  // new size; a hidden host needs a new frame before show().
  [[nodiscard]] Result resize(OverlayBounds bounds);
  // Input is consumed synchronously and never retained. Present a first frame
  // before show().
  [[nodiscard]] Result present(std::span<const std::uint8_t> bgra, unsigned width, unsigned height, std::size_t stride);
  // A failed activation hides the native window again. Passive hosts reject
  // activation.
  //
  // Windows restricts background SetForegroundWindow calls; receiving the
  // last input event is one admission route. Guide is not window input. When plain
  // activation is refused and `foreground_session` names the validated session
  // window (or a window it owns) that currently holds the foreground, Windows
  // hosts start the sibling openxmb-integrations.exe as a short-lived helper.
  // The helper revalidates every identity, probes the session window, attaches
  // its own input queue to activate an invisible helper-owned relay, hands
  // foreground to this host from that foreground process and detaches. This
  // thread never attaches; it waits at most a fixed deadline (dispatching its
  // own window messages) and terminates the helper on expiry. Success means
  // this thread observed the host in the foreground after the helper exited.
  // Pass it only for an explicit user request to open this overlay. X11
  // ignores it: activation remains an EWMH request the window manager may deny.
  [[nodiscard]] Result show(bool activate=true, const std::optional<SessionWindowTarget>& foreground_session={});
  void hide();
  [[nodiscard]] bool visible() const;
  [[nodiscard]] bool focused() const;
  // The monitor beneath the host no longer has the geometry it had at
  // prepare()/resize() (mode, scale or layout change). Windows compares the
  // monitor rectangle; X11 observes root-window size changes only.
  [[nodiscard]] bool display_changed() const;
  [[nodiscard]] std::uintptr_t native_handle() const;
  [[nodiscard]] OverlayStats stats() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace openxmb::integrations
