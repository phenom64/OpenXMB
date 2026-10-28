#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace openxmb::render_detail {
struct FlowCamera {float x,z,yaw,pitch,roll,heave;};
inline FlowCamera flow_camera(double motion_seconds) {
    const auto t=motion_seconds;
    return {static_cast<float>(0.50*std::sin(t*0.08)+0.08*std::sin(t*0.23)),static_cast<float>(-1.2*t),
        static_cast<float>(0.020*std::sin(t*0.17)+0.005*std::sin(t*0.39)),
        static_cast<float>(0.010*std::sin(t*0.13)+0.002*std::sin(t*1.7)),
        static_cast<float>(0.025*std::sin(t*0.11)+0.005*std::sin(t*0.37)),
        static_cast<float>(0.012*std::sin(t*0.31))};
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
