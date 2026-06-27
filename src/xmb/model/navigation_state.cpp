#include "openxmb/xmb/navigation_state.hpp"

#include <new>
#include <utility>

namespace openxmb::xmb {
namespace {

bool requires_identifier(const NavigationEventKind kind) noexcept {
  switch (kind) {
  case NavigationEventKind::select_category:
  case NavigationEventKind::enter_menu:
  case NavigationEventKind::open_modal:
  case NavigationEventKind::open_wizard:
  case NavigationEventKind::advance_wizard:
  case NavigationEventKind::open_osk:
  case NavigationEventKind::open_photo:
  case NavigationEventKind::open_music:
  case NavigationEventKind::open_video:
  case NavigationEventKind::open_option_panel:
    return true;
  default:
    return false;
  }
}

void push(NavigationState &state, NavigationLayer layer,
          const NavigationEvent &event) {
  state.stack.push_back({layer, event.id, event.selection, event.resumable});
}

} // namespace

bool NavigationState::valid() const noexcept {
  if (stack.empty())
    return false;
  if (stack.front().layer == NavigationLayer::boot)
    return stack.size() == 1;
  if (stack.front().layer != NavigationLayer::root)
    return false;
  for (std::size_t i = 1; i < stack.size(); ++i) {
    if (stack[i].layer == NavigationLayer::boot ||
        stack[i].layer == NavigationLayer::root) {
      return false;
    }
    if (stack[i].id.empty())
      return false;
    if (stack[i].layer == NavigationLayer::osk &&
        stack[i - 1].layer != NavigationLayer::wizard) {
      return false;
    }
  }
  return true;
}

NavigationResult transition(const NavigationState &state,
                            const NavigationEvent &event) noexcept {
  if (!state.valid())
    return {std::nullopt, NavigationError::invalid_state, false};
  if (requires_identifier(event.kind) && event.id.empty()) {
    return {std::nullopt, NavigationError::invalid_event, false};
  }

  try {
    NavigationState next = state;
    switch (event.kind) {
    case NavigationEventKind::complete_boot:
      if (next.top().layer != NavigationLayer::boot) {
        return {std::nullopt, NavigationError::invalid_event, false};
      }
      next.stack = {{NavigationLayer::root, "root", event.selection, true}};
      break;
    case NavigationEventKind::select_category:
      if (next.top().layer != NavigationLayer::root) {
        return {std::nullopt, NavigationError::invalid_event, false};
      }
      next.active_category = event.id;
      next.stack.front().selection = event.selection;
      break;
    case NavigationEventKind::enter_menu:
      if (next.top().layer != NavigationLayer::root &&
          next.top().layer != NavigationLayer::nested_menu) {
        return {std::nullopt, NavigationError::invalid_event, false};
      }
      push(next, NavigationLayer::nested_menu, event);
      break;
    case NavigationEventKind::open_modal:
      push(next, NavigationLayer::modal, event);
      break;
    case NavigationEventKind::open_wizard:
      push(next, NavigationLayer::wizard, event);
      break;
    case NavigationEventKind::advance_wizard:
      if (next.top().layer != NavigationLayer::wizard) {
        return {std::nullopt, NavigationError::invalid_event, false};
      }
      push(next, NavigationLayer::wizard, event);
      break;
    case NavigationEventKind::open_osk:
      if (next.top().layer != NavigationLayer::wizard) {
        return {std::nullopt, NavigationError::invalid_event, false};
      }
      push(next, NavigationLayer::osk, event);
      break;
    case NavigationEventKind::open_photo:
      push(next, NavigationLayer::photo, event);
      break;
    case NavigationEventKind::open_music:
      push(next, NavigationLayer::music, event);
      break;
    case NavigationEventKind::open_video:
      push(next, NavigationLayer::video, event);
      break;
    case NavigationEventKind::open_option_panel:
      push(next, NavigationLayer::option_panel, event);
      break;
    case NavigationEventKind::set_selection:
      next.stack.back().selection = event.selection;
      break;
    case NavigationEventKind::back:
      if (next.stack.size() == 1)
        return {std::move(next), NavigationError::none, false};
      next.stack.pop_back();
      // Progress/test wizard routes are not resumable. Back skips them,
      // matching the reference router's priority after closing OSK,
      // option panels, or modals one layer at a time.
      while (next.stack.size() > 1 &&
             next.top().layer == NavigationLayer::wizard &&
             !next.top().resumable) {
        next.stack.pop_back();
      }
      break;
    case NavigationEventKind::reset_to_root:
      if (next.stack.front().layer == NavigationLayer::boot) {
        return {std::nullopt, NavigationError::invalid_event, false};
      }
      next.stack.resize(1);
      break;
    }
    if (!next.valid())
      return {std::nullopt, NavigationError::invalid_state, false};
    const bool changed = next != state;
    return {std::move(next), NavigationError::none, changed};
  } catch (const std::bad_alloc &) {
    return {std::nullopt, NavigationError::allocation_failure, false};
  } catch (...) {
    return {std::nullopt, NavigationError::invalid_event, false};
  }
}

} // namespace openxmb::xmb
