#pragma once
#include <algorithm>
#include <cmath>
namespace programs::detail {
inline float notice_alpha(double elapsed) {
    if(!std::isfinite(elapsed)||elapsed<0)return 0;
    const float progress=static_cast<float>(std::clamp((elapsed-2.2)/0.8,0.0,1.0));
    return 1-progress*progress*(3-2*progress);
}
}
