module;
#include "openxmb/integrations/session_window.hpp"
#include "openxmb/integrations/xmb_overlay.hpp"
#include "openxmb/xmb/overlay_motion.hpp"
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

export module openxmb.app:session_xmb;
import auroreengine;
import vulkan_hpp;
import spdlog;

namespace app {
export class session_xmb {
  auroreengine::window* window_;
  struct State {
    openxmb::integrations::XmbOverlay native;
    // Authorises the native foreground handoff from this session window.
    std::optional<openxmb::integrations::SessionWindowTarget> foreground_session;
    bool shown{};
    bool closing{},reduced{},passive{};
    openxmb::xmb::OverlayMotion motion;
    std::string error;
  };
  std::shared_ptr<State> state_;
  bool active_{};
  // Runs synchronously on the window owner thread after GPU completion.
  static std::function<void(const auroreengine::OffscreenFrame&)> sink(std::shared_ptr<State> state) {
    return [state=std::move(state)](const auroreengine::OffscreenFrame& frame) {
      if(!state->error.empty())return;
      if(frame.format!=vk::Format::eB8G8R8A8Srgb) {state->error="The overlay frame format (VkFormat "+std::to_string(static_cast<int>(frame.format))+") is not B8G8R8A8_SRGB.";state->native.hide();return;}
      const auto delivered=state->native.present(frame.bytes,frame.width,frame.height,frame.row_stride);
      if(!delivered) {state->error=delivered.message;state->native.hide();return;}
      if(!state->shown) {
        const auto activated=state->native.show(!state->passive,state->foreground_session);
        if(!activated) {state->error=activated.message;return;}
        if(!activated.message.empty())spdlog::debug("{}",activated.message);
        state->shown=true;
        state->motion.show(seconds(),state->reduced);
      }
    };
  }
public:
  static double seconds() {return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
  explicit session_xmb(auroreengine::window* value):window_(value) {}
  ~session_xmb() {close();}
  session_xmb(const session_xmb&)=delete;
  session_xmb& operator=(const session_xmb&)=delete;
  [[nodiscard]] bool active() const {return active_;}
  [[nodiscard]] bool passive() const {return state_ && state_->passive;}
  // The native host has shown its first frame.
  [[nodiscard]] bool presented() const {return state_ && state_->shown;}
  [[nodiscard]] bool focused() const {return state_ && state_->native.focused();}
  [[nodiscard]] bool visible() const {return state_ && state_->native.visible();}
  [[nodiscard]] bool closing() const {return state_ && state_->closing;}
  // The monitor under the host changed geometry; call resize() with the
  // session monitor's current bounds from phase::after_events().
  [[nodiscard]] bool display_changed() const {return active_ && state_ && state_->native.display_changed();}
  [[nodiscard]] openxmb::xmb::OverlayMotionSample motion() const {
    return state_?state_->motion.sample(seconds()):openxmb::xmb::OverlayMotionSample{};
  }
  void dismiss(bool reduced=false) {if(state_&&!state_->closing){state_->closing=true;state_->motion.hide(seconds(),reduced);}}
  void reopen(bool reduced=false) {if(state_){state_->closing=false;state_->motion.show(seconds(),reduced);}}
  [[nodiscard]] bool dismissal_finished() const {const auto sample=motion();return closing()&&sample.settled&&sample.opacity<=0;}
  [[nodiscard]] std::string take_error() {return state_?std::exchange(state_->error,{}):std::string{};}
  [[nodiscard]] openxmb::integrations::OverlayStats stats() const {return state_?state_->native.stats():openxmb::integrations::OverlayStats{};}
  // Passive hosts never activate, route input or report focus loss; the
  // callback and foreground session are ignored. An interactive host given
  // `foreground_session` (the validated session window, for an explicit
  // Guide/shortcut request) may hand foreground from that window when Windows
  // refuses plain activation; see XmbOverlay::show.
  openxmb::integrations::Result open(openxmb::integrations::OverlayBounds bounds,openxmb::integrations::OverlayCallback callback,bool reduced=false,
    openxmb::integrations::OverlayInteraction interaction=openxmb::integrations::OverlayInteraction::interactive,
    std::optional<openxmb::integrations::SessionWindowTarget> foreground_session={}) {
    if(active_)return {false,"The in-game XMB is already visible."};
    try {
      state_=std::make_shared<State>();
      state_->reduced=reduced;state_->passive=interaction==openxmb::integrations::OverlayInteraction::passive;
      if(!state_->passive)state_->foreground_session=std::move(foreground_session);
      const auto prepared=state_->native.prepare(bounds,std::move(callback),interaction);
      if(!prepared) {state_.reset();return prepared;}
      window_->begin_offscreen_presentation(vk::Extent2D{bounds.width,bounds.height},sink(state_));
      active_=true;
      return {true,"In-game XMB prepared."};
    } catch(const std::exception& error) {
      state_.reset();active_=false;
      return {false,error.what()};
    }
  }
  // Re-targets the open host to new monitor bounds without recreating its
  // window, so focus and stacking survive. Call from phase::after_events().
  openxmb::integrations::Result resize(openxmb::integrations::OverlayBounds bounds) {
    if(!active_||!state_)return {false,"The in-game XMB is not open."};
    // The engine restores the previous extent if re-entry throws, so the host
    // is only resized after the renderer produces frames of the new size.
    try {window_->begin_offscreen_presentation(vk::Extent2D{bounds.width,bounds.height},sink(state_));}
    catch(const std::exception& error) {return {false,error.what()};}
    auto resized=state_->native.resize(bounds);
    if(!resized) {state_->error=resized.message;state_->native.hide();}
    return resized;
  }
  void hide() {if(state_)state_->native.hide();}
  openxmb::integrations::Result close() noexcept {
    hide();
    if(active_) {
      try {window_->end_offscreen_presentation();}
      catch(const std::exception& error) {return {false,error.what()};}
      catch(...) {return {false,"The in-game XMB could not restore normal presentation."};}
    }
    active_=false;state_.reset();
    return {true,{}};
  }
};
} // namespace app
