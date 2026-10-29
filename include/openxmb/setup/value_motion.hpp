#pragma once
#include <algorithm>

namespace openxmb::setup {
struct ValueMotion {
    float incoming_offset{},outgoing_offset{},incoming_alpha{1},outgoing_alpha{};
};
// The reference has one or two blended frames at 25 fps. Keep the motion
// short and independent of frame rate, with up/down supplied by the action
// rather than inferred from the value (which can wrap).
inline ValueMotion value_motion(float elapsed,int direction,bool reduced=false) {
    if(reduced||elapsed>=0.08F)return {};
    const float t=std::clamp(elapsed/0.08F,0.0F,1.0F);
    const float blend=t*t*(3-2*t),travel=direction<0?-16.0F:16.0F;
    return {-travel*(1-blend),travel*blend,blend,1-blend};
}
}
