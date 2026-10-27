#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
namespace openxmb::render_detail {
struct MusicResponse {
    float bass{},mid{},treble{},energy{},beat{},speed{1};
    double extra_seconds{};
    void advance(std::span<const float> bands,bool playing,double seconds) {
        const auto dt=static_cast<float>(std::clamp(std::isfinite(seconds)?seconds:0.0,0.0,0.1));
        const auto envelope=[dt](float current,float target,float attack=0.025F,float release=0.22F){return current+(target-current)*(1-std::exp(-dt/(target>current?attack:release)));};
        const auto strength=[&](std::size_t first,std::size_t end){float peak=0;for(std::size_t i=first;i<std::min(end,bands.size());++i)if(std::isfinite(bands[i]))peak=std::max(peak,bands[i]);return playing?std::pow(std::clamp(peak*3.0F,0.0F,1.0F),0.65F):0.0F;};
        const auto low=strength(0,8),middle=strength(8,16),high=strength(16,24);
        const auto onset=playing?std::clamp((low-bass_history-0.025F)*2.6F,0.0F,1.0F):0.0F;
        bass_history=envelope(bass_history,low,0.4F,0.45F);
        bass=envelope(bass,low);mid=envelope(mid,middle);treble=envelope(treble,high);
        energy=envelope(energy,std::max({low*0.85F,middle*0.70F,high*0.45F}));
        beat=envelope(beat,onset,0.012F,0.16F);
        speed=1+(playing?0.28F+energy*0.65F+beat*0.12F:0);
        extra_seconds+=dt*(speed-1);
    }
private:float bass_history{};
};
}
