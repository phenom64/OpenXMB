#define SDL_MAIN_HANDLED
#include <openxmb/video.hpp>
#include <openxmb/video_worker.hpp>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

int main(int argc,char** argv) {
  namespace video=openxmb::video;
  using namespace std::chrono_literals;
  const auto check=[](bool condition,const char* message){if(!condition)throw std::runtime_error(message);};
  try {
    check(argc==2,"A video fixture is required");
    const video::options settings{.hardware_acceleration="off",.audio_output=false,.native_yuv=true};
    video::session direct(argv[1],settings);direct.pause(true);
    bool first=false;
    for(int i=0;i<200&&!first;++i) {direct.pump();if(auto frame=direct.take_frame())first=frame->width>0&&(!frame->rgba.empty()||!frame->planes[0].bytes.empty());}
    check(first&&direct.playback_state()==video::state::paused,"Paused-before-first-pump decoder did not publish initial still frame");
    const auto at=direct.position();std::this_thread::sleep_for(100ms);direct.pump();
    check(std::abs(direct.position()-at)<0.001&&!direct.take_frame(),"Paused initial still frame advanced");
    video::worker worker;worker.set_path(argv[1],settings);worker.pause(true);
    auto deadline=std::chrono::steady_clock::now()+3s;bool ready=false;double first_pts{};
    while(std::chrono::steady_clock::now()<deadline) {
      if(worker.snapshot()->status==video::state::failed)throw std::runtime_error(worker.snapshot()->error);
      if(worker.snapshot()->ready&&worker.snapshot()->status==video::state::paused) {
        if(auto frame=worker.take_frame()) {ready=frame->width>0&&(!frame->rgba.empty()||!frame->planes[0].bytes.empty());first_pts=frame->timestamp;break;}
      }
      std::this_thread::sleep_for(5ms);
    }
    check(ready,"Background worker paused at open did not publish a native initial frame");
    const auto frozen=worker.snapshot()->position;std::this_thread::sleep_for(150ms);
    check(std::abs(worker.snapshot()->position-frozen)<0.001&&!worker.take_frame(),"Background still frame did not remain frozen");
    worker.pause(false);deadline=std::chrono::steady_clock::now()+2s;bool resumed=false;
    while(std::chrono::steady_clock::now()<deadline) {
      if(auto frame=worker.take_frame();frame&&frame->timestamp>first_pts+0.001) {resumed=true;break;}
      std::this_thread::sleep_for(5ms);
    }
    check(resumed,"Clearing reduced-motion pause did not resume frame progression");
    std::cout<<"video_reduced_motion_tests: paused-before-first-pump still, background initial frame, frozen retention and resume passed\n";
    return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
