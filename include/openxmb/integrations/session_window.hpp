#pragma once
#include "openxmb/integrations/desktop.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>

namespace openxmb::integrations {
// A process creation time and HWND identify one candidate, not an ownership
// capability. Establish a live session process identity before using this target.
struct SessionWindowTarget {
  std::int64_t pid{};
  std::uintptr_t handle{};
  std::uint64_t process_creation{};
};
struct SessionWindowBounds {int x{},y{};unsigned width{},height{};};
// Current visible client geometry, after validating the pinned identity.
[[nodiscard]] std::optional<SessionWindowBounds> session_window_bounds(const SessionWindowTarget& target);
// Returns no target when no unique eligible window exists.
[[nodiscard]] std::optional<SessionWindowTarget> find_session_window(std::int64_t pid);
[[nodiscard]] Result validate_session_window(const SessionWindowTarget& target);
// Foreground activation is subject to OS policy and may be denied.
[[nodiscard]] Result focus_session_window(const SessionWindowTarget& target);
// Submit without waiting for another process/window manager to activate it.
// Success means the request was submitted; observe session_window_focused()
// separately. Used by launch handoffs on the UI thread.
[[nodiscard]] Result request_session_window_focus(const SessionWindowTarget& target);
// Read-only, identity-checked observations for an asynchronous launch handoff.
[[nodiscard]] bool session_window_focused(const SessionWindowTarget& target);
// True only when the session client covers a visible window owned by this
// process. Windowed applications keep the XMB backdrop behind them.
[[nodiscard]] bool session_window_covers(const SessionWindowTarget& target,std::uintptr_t own_window);
// PNG capture of the foreground client. Windows Graphics Capture isolates the
// exact HWND; unsupported systems and X11 require an unobstructed visible client.
// A valid hidden/minimized
// shell HWND owned by this process is mandatory. The caller must revalidate its
// session capability and pinned identity. Occlusion/protected/black content can be unavailable;
// Cancellation is checked before native capture and immediately before atomic
// publication; a cancelled request must not publish a later PNG.
enum class CaptureState { saved, pending, unavailable };
struct SessionCaptureResult {
  CaptureState state{CaptureState::unavailable};
  std::string message;
  explicit operator bool() const noexcept {return state==CaptureState::saved;}
};
[[nodiscard]] SessionCaptureResult capture_session_window(const SessionWindowTarget& target,
  const std::filesystem::path& destination, std::uintptr_t excluded_shell_handle = 0,
  std::stop_token cancellation = {});
} // namespace openxmb::integrations
