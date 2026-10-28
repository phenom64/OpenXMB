#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
namespace openxmb::render_detail {
// UI-side onset timing, never an audio callback. Confidence keeps irregular
// attacks/ambiguous music on the energy fallback instead of inventing a BPM.
struct FlowTempo {
    float speed{1.0F},bpm{},confidence{};
    void advance(double elapsed,float beat,float energy,bool playing,std::uint64_t generation) {
        if(!initialized_||track_!=generation) { *this=FlowTempo{};initialized_=true;track_=generation; }
        const double dt=std::clamp(std::isfinite(elapsed)?elapsed:0.0,0.0,0.1);
        time_+=dt;
        beat=std::isfinite(beat)?std::max(0.0F,beat):0;
        energy=std::isfinite(energy)?std::clamp(energy,0.0F,1.0F):0;
        if(playing&&previous_>older_&&previous_>=beat&&previous_>0.055F&&time_-last_peak_>0.25) {
            const double interval=time_-last_peak_;
            if(last_peak_>=0&&interval>=0.28&&interval<=1.6) {
                intervals_[next_++%intervals_.size()]=interval;
                count_=std::min(count_+1,intervals_.size());
                if(count_>=3) {
                    auto sorted=intervals_;std::sort(sorted.begin(),sorted.begin()+count_);
                    const auto median=sorted[count_/2];
                    double error=0;for(std::size_t i=0;i<count_;++i)error+=std::abs(intervals_[i]-median)/median;
                    confidence=static_cast<float>(std::clamp(1.0-error/count_*4.0,0.0,1.0));
                    bpm=static_cast<float>(60.0/median);
                }
            } else if(last_peak_>=0) {count_=next_=0;confidence=0;}
            last_peak_=time_;
        }
        older_=previous_;previous_=beat;
        if(!playing||time_-last_peak_>2.2)confidence*=static_cast<float>(std::exp(-dt/0.8));
        const float fallback=1.08F+energy*0.50F;
        const float tempo=std::clamp(0.72F+(bpm-60.0F)/120.0F*1.30F,0.72F,2.15F)+energy*0.12F;
        const float blend=confidence>0.65F?confidence:0;
        const float target=playing?std::clamp(fallback+(tempo-fallback)*blend,0.72F,2.3F):1.0F;
        speed+=(target-speed)*static_cast<float>(1.0-std::exp(-dt/1.8));
    }
private:
    std::array<double,8> intervals_{};
    std::size_t count_{},next_{};
    double time_{},last_peak_{-10};
    float previous_{},older_{};
    std::uint64_t track_{};
    bool initialized_{};
};
}
