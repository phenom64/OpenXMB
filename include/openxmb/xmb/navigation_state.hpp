#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openxmb::xmb {

enum class NavigationLayer {
  boot,
  root,
  nested_menu,
  modal,
  wizard,
  osk,
  photo,
  music,
  video,
  option_panel,
};

struct NavigationRoute {
  NavigationLayer layer{NavigationLayer::boot};
  std::string id;
  std::size_t selection{};
  bool resumable{true};

  friend bool operator==(const NavigationRoute &,
                         const NavigationRoute &) = default;
};

struct NavigationState {
  std::vector<NavigationRoute> stack{{NavigationLayer::boot, "boot", 0, true}};
  std::string active_category;

  [[nodiscard]] const NavigationRoute &top() const noexcept {
    return stack.back();
  }
  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const NavigationState &,
                         const NavigationState &) = default;
};

enum class NavigationEventKind {
  complete_boot,
  select_category,
  enter_menu,
  open_modal,
  open_wizard,
  advance_wizard,
  open_osk,
  open_photo,
  open_music,
  open_video,
  open_option_panel,
  set_selection,
  back,
  reset_to_root,
};

struct NavigationEvent {
  NavigationEventKind kind{NavigationEventKind::back};
  std::string id;
  std::size_t selection{};
  bool resumable{true};
};

enum class NavigationError {
  none,
  invalid_state,
  invalid_event,
  allocation_failure
};

struct NavigationResult {
  std::optional<NavigationState> state;
  NavigationError error{NavigationError::none};
  bool changed{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == NavigationError::none;
  }
};

// Pure state transition: input is never modified and every failure returns the
// original state. Failed transitions contain no replacement state, so even an
// allocation failure can be reported without trying to copy the input again.
[[nodiscard]] NavigationResult
transition(const NavigationState &state, const NavigationEvent &event) noexcept;

} // namespace openxmb::xmb
