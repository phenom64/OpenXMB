#pragma once
#include <algorithm>
#include <cmath>
namespace app::media_motion {
inline double ease(double fraction) {const auto p=std::clamp(fraction,0.0,1.0);return 1-std::pow(1-p,3);}
inline double category(unsigned from,unsigned to,double fraction) {return from&&to?1:to?ease(fraction):1-ease(fraction);}
inline float backdrop(float amount,bool opaque_scene) {return opaque_scene?0:amount;}
struct Layers {double outgoing_x,incoming_x,outgoing_alpha,incoming_alpha;};
inline Layers layers(bool returning,double fraction) {const auto p=ease(fraction);const auto direction=returning?-1.0:1.0;return {-250*direction*p,250*direction*(1-p),1-p,p};}
}
