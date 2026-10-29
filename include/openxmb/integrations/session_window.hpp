#pragma once
#include "openxmb/integrations/desktop.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>

namespace openxmb::integrations {
// A process creation time and HWND identify one candidate, not an ownership
// capability. Call only for a live session tracked as an owned direct child.
struct SessionWindowTarget {
  std::int64_t pid{};
  std::uintptr_t handle{};
  std::uint64_t process_creation{};
};
// Returns no target when no unique eligible window exists.
[[nodiscard]] std::optional<SessionWindowTarget> find_session_window(std::int64_t pid);
[[nodiscard]] Result validate_session_window(const SessionWindowTarget& target);
// Foreground activation is subject to OS policy and may be denied.
[[nodiscard]] Result focus_session_window(const SessionWindowTarget& target);
// PNG capture of the visible foreground client only. A valid hidden/minimized
// shell HWND owned by this process is mandatory. The caller must revalidate its
// direct-child session. Occlusion/protected/black content can be unavailable;
// no desktop capture can guarantee a live GPU frame or rule out every overlay.
[[nodiscard]] Result capture_session_window(const SessionWindowTarget& target,
  const std::filesystem::path& destination, std::uintptr_t excluded_shell_handle = 0);
} // namespace openxmb::integrations
