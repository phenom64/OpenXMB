#pragma once
#include <array>
#include <cstdint>

namespace app {
// SDL is the fallback until native delivery is proven. Native then owns Guide
// for this registration, avoiding duplicate completed taps from two sources.
// Both sources supply steady-clock microseconds; focus changes do not reset it.
class GuideTiming {
public:
    enum class Source {sdl,native};
    bool edge(Source source,bool pressed,std::uint64_t timestamp) {
        const auto index=static_cast<unsigned>(source);
        if(seen_[index]&&timestamp<last_[index])return false;
        seen_[index]=true;last_[index]=timestamp;
        if(source==Source::sdl) {
            if(native_authority_)return false;
            if(held_[index]==pressed)return false;
            held_[index]=pressed;
            if(pressed){sdl_press_=timestamp;sdl_pressed_=true;}
            return pressed;
        }
        if(!native_authority_) {
            native_authority_=true;
            // An in-flight SDL hold represents the same handover gesture.
            suppress_native_hold_=held_[0];
        }
        const bool was_held=held_[index];held_[index]=pressed;
        if(!pressed){suppress_native_hold_=false;return false;}
        if(was_held||suppress_native_hold_)return false;
        return !sdl_pressed_||timestamp>sdl_press_;
    }
    void reset() {*this=GuideTiming{};}
private:
    std::array<bool,2> held_{},seen_{};
    std::array<std::uint64_t,2> last_{};
    bool native_authority_{},suppress_native_hold_{},sdl_pressed_{};
    std::uint64_t sdl_press_{};
};
}
