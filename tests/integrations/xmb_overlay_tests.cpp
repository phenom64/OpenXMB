#include "openxmb/integrations/xmb_overlay.hpp"
#include "openxmb/xmb/overlay_motion.hpp"
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
using namespace openxmb::integrations;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){try{
  openxmb::xmb::OverlayMotion motion;
  require(motion.sample(10).opacity==0,"unpresented overlay must remain transparent");
  motion.show(10);const auto midway=motion.sample(10.12);
  require(midway.opacity>0&&midway.opacity<1&&midway.zoom>1&&midway.zoom<1.12F,"entrance must fade and zoom");
  require(motion.sample(10.25).opacity==1&&motion.sample(10.25).zoom==1,"entrance must settle exactly");
  motion.hide(11);const auto leaving=motion.sample(11.06);
  motion.show(11.06);require(std::abs(motion.sample(11.06).opacity-leaving.opacity)<0.0001F,"rapid reopening must not jump opacity");
  motion.hide(12,true);require(motion.sample(12).opacity==0,"reduced motion must dismiss immediately");
  const std::array<std::uint8_t,20> input{255,125,3,0, 137,137,137,64, 188,188,188,128, 92,0,0,128, 73,99,40,255};
  std::array<std::uint8_t,20> output{};
  require(bool(convert_overlay_bgra(input,5,1,20,output,20)),"pixel conversion failed");
  require(output[0]==0&&output[1]==0&&output[2]==0&&output[3]==0,"transparent pixel retained colour");
  require(output[4]==64&&output[5]==64&&output[6]==64&&output[7]==64,"25 percent white");
  require(output[8]==128&&output[9]==128&&output[10]==128&&output[11]==128,"50 percent white");
  require(output[12]>=63&&output[12]<=65&&output[13]==0&&output[14]==0&&output[15]==128,"colour conversion or channel order");
  require(output[16]==73&&output[17]==99&&output[18]==40&&output[19]==255,"opaque pixel changed");
  std::array<std::uint8_t,24> padded_in{};padded_in[3]=255;padded_in[12]=188;padded_in[13]=188;padded_in[14]=188;padded_in[15]=128;
  std::array<std::uint8_t,24> padded_out{};padded_out.fill(17);
  require(bool(convert_overlay_bgra(padded_in,1,2,12,padded_out,12)),"padded rows failed");
  require(padded_out[3]==255&&padded_out[12]==128&&padded_out[15]==128&&padded_out[4]==17,"row stride corrupted padding");
  const auto before=padded_out;
  require(!convert_overlay_bgra(padded_in,0,2,12,padded_out,12),"zero width accepted");
  require(!convert_overlay_bgra(padded_in,2,2,7,padded_out,12),"short input stride accepted");
  require(!convert_overlay_bgra(padded_in,2,2,12,padded_out,7),"short output stride accepted");
  require(!convert_overlay_bgra(padded_in,2,3,12,padded_out,12),"short input buffer accepted");
  require(!convert_overlay_bgra(padded_in,std::numeric_limits<unsigned>::max(),2,12,padded_out,12),"oversized width accepted");
  require(padded_out==before,"invalid request modified output");
  require(!convert_overlay_bgra(padded_out,1,2,12,padded_out,12),"overlapping input and output accepted");
#ifndef _WIN32
  XmbOverlay overlay;require(!overlay.prepare({0,0,100,100},{}),"unsupported platform reported available");
#else
  const auto foreground=GetForegroundWindow();
  XmbOverlay overlay;
  require(bool(overlay.prepare({-32000,-32000,32,16},{})),"hidden native compositor prepare failed");
  require(!overlay.visible()&&!overlay.focused(),"hidden host changed desktop focus");
  require(!overlay.show(),"uninitialized frame could be shown");
  std::vector<std::uint8_t> frame(32*16*4);
  for(std::size_t i=0;i<frame.size();i+=4)frame[i+3]=128;
  require(!overlay.present(frame,31,16,32*4),"mismatched frame dimensions accepted");
  require(bool(overlay.present(frame,32,16,32*4)),"hidden UpdateLayeredWindow present failed");
  require(!overlay.visible()&&overlay.stats().frames==1,"present implicitly showed the host or lost its frame");
  bool wrong_thread_rejected=false;
  std::thread other([&]{wrong_thread_rejected=!overlay.present(frame,32,16,32*4);});other.join();
  require(wrong_thread_rejected,"native presentation accepted another thread");
  overlay.hide();
  require(GetForegroundWindow()==foreground,"hidden compositor changed foreground ownership");
#endif
  std::cout<<"Overlay conversion fixtures passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
