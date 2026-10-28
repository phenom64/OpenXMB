#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace openxmb::render_detail {
struct FlowCamera {float x,z;};
inline FlowCamera flow_camera(double motion_seconds) {
    return {static_cast<float>(0.35*std::sin(motion_seconds*0.023)),static_cast<float>(-0.85*motion_seconds)};
}
struct FlowClock {
    double motion{},scene{};
    std::uint64_t generation{};
    bool initialized{};
    void advance(double elapsed,float speed,std::uint64_t track) {
        if(!initialized||generation!=track) {motion=scene=0;generation=track;initialized=true;}
        const auto dt=std::clamp(std::isfinite(elapsed)?elapsed:0.0,0.0,0.1);
        scene+=dt;motion+=dt*std::clamp(std::isfinite(speed)?speed:1.0F,0.0F,3.0F);
    }
};
}
