#pragma once
#include <algorithm>
#include <cmath>

namespace openxmb::xmb {
struct OverlayMotionSample { float opacity{},zoom{1.12F}; bool settled{true}; };
// A reversible presentation envelope. Time starts at the first presented
// frame, so pipeline preparation never consumes the visible entrance.
class OverlayMotion {
public:
    [[nodiscard]] OverlayMotionSample sample(double now) const {
        const double elapsed=std::isfinite(now)?std::max(0.0,now-began_):0.0;
        const double t=duration_>0?std::clamp(elapsed/duration_,0.0,1.0):1.0;
        const double eased=1.0-(1.0-t)*(1.0-t)*(1.0-t);
        const float value=static_cast<float>(from_+(target_-from_)*eased);
        return {value,1.12F-0.12F*value,t>=1.0};
    }
    void show(double now,bool reduced=false) {retarget(1.0,now,reduced?0.0:0.24);}
    void hide(double now,bool reduced=false) {retarget(0.0,now,reduced?0.0:0.18);}
private:
    double from_{},target_{},began_{},duration_{};
    void retarget(double target,double now,double duration) {
        from_=sample(now).opacity;target_=target;
        began_=std::isfinite(now)?now:0.0;duration_=duration*std::abs(target_-from_);
    }
};
}
