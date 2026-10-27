#include "../../src/render/music_response.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
void check(bool b,const char* m){if(!b)throw std::runtime_error(m);}
int main()try{using openxmb::render_detail::MusicResponse;std::array<float,24> zero{},low{},high{};low[2]=0.12F;high[21]=0.12F;
 MusicResponse idle;for(int i=0;i<120;++i)idle.advance(zero,false,1.0/120);
 check(idle.speed==1&&idle.extra_seconds==0&&idle.energy==0&&idle.beat==0,"idle wave remains exactly unchanged");
 MusicResponse bass,treble;for(int i=0;i<8;++i){bass.advance(low,true,1.0/60);treble.advance(high,true,1.0/60);}
 check(bass.bass>0.45F&&bass.energy>0.35F&&bass.beat>0.1F,"a narrow musical bass band must produce visible response, not be divided by 24");
 check(treble.treble>0.45F&&treble.bass==0,"bass and treble remain distinct");
 check(bass.speed>1.5F&&bass.speed<2.2F,"playing music accelerates smoothly within calm range");
 MusicResponse a,b;for(int i=0;i<30;++i)a.advance(low,true,1.0/30);for(int i=0;i<120;++i)b.advance(low,true,1.0/120);
 check(std::abs(a.bass-b.bass)<0.001F&&std::abs(a.energy-b.energy)<0.001F,"response envelopes must not depend on render FPS");
 const auto before=bass.bass;bass.advance(zero,false,1.0/60);check(bass.bass<before&&bass.bass>0,"pause response releases without snapping");
 for(int i=0;i<180;++i)bass.advance(zero,false,1.0/60);
 check(bass.energy<0.001F&&bass.beat<0.001F&&bass.speed==1,"pause/silence settle to idle with no fake beats");
 auto bad=zero;bad[2]=std::numeric_limits<float>::quiet_NaN();bad[8]=std::numeric_limits<float>::infinity();MusicResponse safe;safe.advance(bad,true,0.1);check(std::isfinite(safe.energy)&&safe.energy==0,"nonfinite input cannot poison GPU uniforms");
 std::cout<<"Audio band separation, onset, idle invariance and time-based response passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
