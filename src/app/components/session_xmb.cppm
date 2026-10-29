module;
#include "openxmb/integrations/xmb_overlay.hpp"
#include "openxmb/xmb/overlay_motion.hpp"
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>

export module openxmb.app:session_xmb;
import auroreengine;
import vulkan_hpp;

namespace app {
export class session_xmb {
  auroreengine::window* window_;
  struct State {
    openxmb::integrations::XmbOverlay native;
    bool shown{};
    bool closing{},reduced{},passive{};
    openxmb::xmb::OverlayMotion motion;
    std::string error;
  };
  std::shared_ptr<State> state_;
  bool active_{};
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
  [[nodiscard]] openxmb::xmb::OverlayMotionSample motion() const {
    return state_?state_->motion.sample(seconds()):openxmb::xmb::OverlayMotionSample{};
  }
  void dismiss(bool reduced=false) {if(state_&&!state_->closing){state_->closing=true;state_->motion.hide(seconds(),reduced);}}
  void reopen(bool reduced=false) {if(state_){state_->closing=false;state_->motion.show(seconds(),reduced);}}
  [[nodiscard]] bool dismissal_finished() const {const auto sample=motion();return closing()&&sample.settled&&sample.opacity<=0;}
  [[nodiscard]] std::string take_error() {return state_?std::exchange(state_->error,{}):std::string{};}
  [[nodiscard]] openxmb::integrations::OverlayStats stats() const {return state_?state_->native.stats():openxmb::integrations::OverlayStats{};}
  // Passive hosts never activate, route input or report focus loss; the
  // callback is ignored.
  openxmb::integrations::Result open(openxmb::integrations::OverlayBounds bounds,openxmb::integrations::OverlayCallback callback,bool reduced=false,
    openxmb::integrations::OverlayInteraction interaction=openxmb::integrations::OverlayInteraction::interactive) {
    if(active_)return {false,"The in-game XMB is already visible."};
    try {
      state_=std::make_shared<State>();
      state_->reduced=reduced;state_->passive=interaction==openxmb::integrations::OverlayInteraction::passive;
      const auto prepared=state_->native.prepare(bounds,std::move(callback),interaction);
      if(!prepared) {state_.reset();return prepared;}
      state_->error.clear();state_->shown=false;
      window_->begin_offscreen_presentation(vk::Extent2D{bounds.width,bounds.height},[state=state_](const auroreengine::OffscreenFrame& frame) {
        if(!state->error.empty())return;
        if(frame.format!=vk::Format::eB8G8R8A8Srgb) {state->error="The overlay frame format is unavailable.";return;}
        const auto delivered=state->native.present(frame.bytes,frame.width,frame.height,frame.row_stride);
        if(!delivered) {state->error=delivered.message;state->native.hide();return;}
        if(!state->shown) {
          const auto activated=state->native.show(!state->passive);
          if(!activated) {state->error=activated.message;return;}
          state->shown=true;
          state->motion.show(seconds(),state->reduced);
        }
      });
      active_=true;
      return {true,"In-game XMB prepared."};
    } catch(const std::exception& error) {
      state_.reset();active_=false;
      return {false,error.what()};
    }
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
