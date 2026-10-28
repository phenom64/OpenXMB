#include "../../src/render/flow_scene.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
int main() try {
    using namespace openxmb::render_detail;
    FlowClock calm,loud;
    for(int i=0;i<7500;++i){calm.advance(.1,1,1);loud.advance(.1,2,1);}
    require(std::abs(calm.scene-750)<.001&&std::abs(loud.scene-calm.scene)<.001,"audio speed cannot shorten long scene dwell");
    require(loud.motion>calm.motion*1.99,"audio still modulates continuous motion");
    const auto first=flow_camera(0),later=flow_camera(4);
    require(later.z<first.z,"camera retreats along negative world Z");
    for(int i=0;i<60000;++i) {
        const auto camera=flow_camera(i/60.0);
        require(std::isfinite(camera.x)&&std::isfinite(camera.z),"finite camera path");
        require(std::abs(camera.yaw)<.026F&&std::abs(camera.pitch)<.013F&&std::abs(camera.roll)<.031F,"small bounded flight rotations");
        require(std::abs(camera.heave)<.013F,"gentle vertical flight sway");
        if(i) {
            const auto previous=flow_camera((i-1)/60.0);
            require(camera.z<previous.z,"no reversal during retreat");
            require(std::abs(camera.roll-previous.roll)<.001F,"no camera angle snapping");
        }
    }
    const float initial_projection=2/(8-first.z),later_projection=2/(8-later.z);
    require(later_projection<initial_projection,"frozen landmark contracts towards vanishing point during retreat");
    const auto before=loud.scene;loud.advance(0,1,1);
    require(loud.scene==before,"viewer reentry without track change preserves phase");
    loud.advance(.1,1,2);require(loud.scene<.11,"new track starts a fresh scene");
    std::cout<<"Flow camera/clocks passed: retreat, landmark contraction, independent long scene timing, reentry\n";
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
