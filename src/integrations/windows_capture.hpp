#pragma once
#include "openxmb/integrations/session_window.hpp"
#include <vector>

namespace openxmb::integrations::windows_capture {
struct Frame {
  bool supported{};
  SessionCaptureResult result{CaptureState::unavailable,"Native window capture is unavailable."};
  std::vector<unsigned char> bgra;
};
// Bounded one-shot Windows Graphics Capture of one HWND, cropped to `client` (physical screen
// pixels), independent of other windows. Call from a worker thread that is COM-uninitialized or
// MTA: frame and GPU readback waits are bounded but not instant. STA callers fail explicitly.
// The first call takes a process-lifetime MTA usage reference (see windows_capture.cpp).
// supported=false only when the OS lacks the API; otherwise result explains any failure and
// bgra holds client.width*client.height opaque SDR BGRA pixels when result is saved.
Frame acquire(std::uintptr_t handle,SessionWindowBounds client);
}
