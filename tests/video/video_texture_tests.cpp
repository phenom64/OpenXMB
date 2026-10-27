#include <openxmb/video.hpp>
#include "../../src/render/video_color.hpp"
#include "../../src/programs/video_preferences.hpp"
#include <iostream>
#include <stdexcept>

int main() {
  namespace video=openxmb::video;
  const auto check=[](bool value,const char* message){if(!value) throw std::runtime_error(message);};
  try {
    using programs::video_preferences::focus_action;
    programs::video_preferences::focus_policy focus;
    check(focus.update(false,false,video::state::playing)==focus_action::pause,"focus loss did not pause playing video");
    check(focus.update(false,false,video::state::playing)==focus_action::none,"stale worker snapshot queued duplicate focus pauses");
    check(focus.update(false,false,video::state::paused)==focus_action::none,"unfocused video resumed");
    check(focus.update(true,false,video::state::paused)==focus_action::resume,"focus return did not resume policy-paused video");
    check(focus.update(true,false,video::state::playing)==focus_action::none,"focus return queued duplicate resume");
    check(focus.update(false,false,video::state::paused)==focus_action::none,"already paused video acquired focus-pause ownership");
    check(focus.update(true,false,video::state::paused)==focus_action::none,"focus return resumed manually paused video");
    check(focus.update(false,true,video::state::playing)==focus_action::none,"background-enabled playback paused on focus loss");
    focus.update(false,false,video::state::playing);focus.cancel();
    check(focus.update(true,false,video::state::paused)==focus_action::none,"manual pause did not cancel automatic resume");
    focus.update(false,false,video::state::playing);
    check(focus.update(true,false,video::state::ended)==focus_action::none,"focus return restarted ended video");
    focus.update(false,false,video::state::playing);
    check(focus.update(true,false,video::state::playing)==focus_action::resume,"fast focus return did not balance in-flight pause command");
    check(std::abs(programs::video_preferences::output_gain(0.5F,80,40)-0.16F)<0.0001F,"video/master gain replaced local volume");
    check(programs::video_preferences::output_gain(1,100,100)==1,"default gain attenuated playback");
    check(programs::video_preferences::language_track({{2,"Audio (eng)"},{3,"Audio (fra)"}},"fra")==3,"preferred language track not found");
    video::frame picture;picture.width=5;picture.height=3;picture.format=video::pixel_format::yuv420p;
    picture.planes[0]={5,3,7,std::vector<std::uint8_t>(21,16)};
    picture.planes[1]={3,2,4,std::vector<std::uint8_t>(8,128)};
    picture.planes[2]=picture.planes[1];
    const auto layout=render::video_detail::layout(picture);
    check(layout.row_bytes[0]==5&&layout.row_bytes[1]==3&&layout.heights[1]==2,"odd dimensions or padded strides lost");
    check(layout.offsets[1]%4==0&&layout.offsets[2]%4==0,"plane offsets not copy aligned");
    auto rgba=render::video_detail::rgba_fallback(picture);
    check(rgba.size()==60&&rgba[0]==0&&rgba[1]==0&&rgba[2]==0&&rgba[3]==255,"limited black conversion failed");
    for(auto matrix:{video::color_matrix::bt601,video::color_matrix::bt709,video::color_matrix::bt2020}) {
      picture.matrix=matrix;
      for(auto& value:picture.planes[0].bytes) value=235;
      rgba=render::video_detail::rgba_fallback(picture);
      check(rgba[0]==255&&rgba[1]==255&&rgba[2]==255,"limited white conversion failed");
      picture.full_range=true;
      for(auto& value:picture.planes[0].bytes) value=128;
      rgba=render::video_detail::rgba_fallback(picture);
      check(rgba[0]==128&&rgba[1]==128&&rgba[2]==128,"full neutral conversion failed");
      picture.full_range=false;
    }
    picture.format=video::pixel_format::nv12;
    picture.planes[1]={3,2,8,std::vector<std::uint8_t>(16,128)};
    picture.planes[2]={};
    const auto nv12=render::video_detail::layout(picture);
    check(nv12.count==2&&nv12.row_bytes[1]==6,"NV12 chroma texels/stride incorrect");
    picture.planes[1].bytes.resize(2);
    bool rejected=false;
    try {static_cast<void>(render::video_detail::layout(picture));}catch(const std::exception&){rejected=true;}
    check(rejected,"truncated native plane accepted");
    std::cout<<"video_texture_tests: odd/padded planes, copy alignment, ranges/matrices and malformed-frame checks passed\n";return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
