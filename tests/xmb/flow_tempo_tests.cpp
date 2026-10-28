#include "../../src/render/flow_tempo.hpp"
#include "../../src/render/music_response.hpp"
#include "../../src/programs/transient_notice.hpp"
#include <iostream>
#include <stdexcept>
using openxmb::render_detail::FlowTempo;
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
FlowTempo simulate(double bpm,int fps) {
    FlowTempo tempo;
    openxmb::render_detail::MusicResponse response;
    for(int i=0;i<fps*22;++i) {
        const double phase=std::fmod(static_cast<double>(i)/fps,60/bpm);
        std::array<float,24> bands{};bands[2]=static_cast<float>(0.006+0.15*std::exp(-phase*phase/0.003));
        response.advance(bands,true,1.0/fps);
        tempo.advance(1.0/fps,response.beat,response.energy,true,1);
    }
    return tempo;
}
int main() try {
    const auto slow=simulate(60,60),fast=simulate(180,60),other=simulate(120,120),normal=simulate(120,30);
    check(slow.confidence>.8&&fast.confidence>.8,"regular actual band onsets produce confident tempo");
    check(std::abs(slow.bpm-60)<3&&std::abs(fast.bpm-180)<7,"slow and fast beat periods distinguished");
    check(fast.speed>slow.speed*1.7&&slow.speed<1,"fast tracks faster; slow tracks remain calm");
    check(std::abs(other.speed-normal.speed)<.12,"motion response stable across frame rates");
    FlowTempo silence;for(int i=0;i<600;++i)silence.advance(.016,0,.2F,true,1);
    check(silence.confidence==0&&silence.bpm==0&&silence.speed<1.3,"no invented tempo for sustained sound");
    auto paused=fast;for(int i=0;i<600;++i)paused.advance(.016,0,0,false,1);
    check(paused.confidence<.001&&std::abs(paused.speed-1)<.01,"pause releases tempo motion smoothly");
    auto reset=fast;reset.advance(.016,0,0,true,2);
    check(reset.confidence==0&&reset.bpm==0,"new track cannot inherit old tempo");
    using programs::detail::notice_alpha;
    check(notice_alpha(0)==1&&notice_alpha(2)==1,"visualiser name held initially");
    check(notice_alpha(2.6)>.45&&notice_alpha(2.6)<.55,"visualiser name fades smoothly");
    check(notice_alpha(3)==0&&notice_alpha(30)==0,"visualiser name disappears after three seconds");
    std::cout<<"Flow tempo and transient label passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
