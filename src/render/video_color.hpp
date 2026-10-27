#pragma once
#include <openxmb/video.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace render::video_detail {
struct upload_layout {
  int count{};
  std::array<std::size_t,3> offsets{},row_bytes{};
  std::array<int,3> heights{};
  std::size_t size{};
};
inline upload_layout layout(const openxmb::video::frame& frame) {
  using openxmb::video::pixel_format;
  if(frame.width<=0||frame.height<=0||frame.width>32768||frame.height>32768) throw std::runtime_error("Invalid video frame dimensions");
  upload_layout out;
  if(frame.format==pixel_format::rgba) {
    out.count=1;out.row_bytes[0]=static_cast<std::size_t>(frame.width)*4;out.heights[0]=frame.height;
    out.size=out.row_bytes[0]*frame.height;
    if(frame.rgba.size()<out.size) throw std::runtime_error("Truncated RGBA video frame");
    return out;
  }
  out.count=frame.format==pixel_format::nv12?2:3;
  for(int i=0;i<out.count;++i) {
    const auto& plane=frame.planes[i];
    const int width=i?(frame.width+1)/2:frame.width,height=i?(frame.height+1)/2:frame.height;
    const std::size_t row=static_cast<std::size_t>(width)*(i==1&&frame.format==pixel_format::nv12?2:1);
    if(plane.width!=width||plane.height!=height||plane.stride<0||static_cast<std::size_t>(plane.stride)<row||
       plane.bytes.size()<static_cast<std::size_t>(plane.stride)*(height-1)+row) throw std::runtime_error("Truncated or invalid native video plane");
    out.offsets[i]=(out.size+3)&~std::size_t(3);
    out.row_bytes[i]=row;out.heights[i]=height;out.size=out.offsets[i]+row*height;
  }
  return out;
}
struct conversion {
  float kr,kb,y_offset,y_scale,chroma_scale;
  std::uint32_t nv12,width,height;
};
static_assert(sizeof(conversion)==32);
inline conversion parameters(const openxmb::video::frame& frame) {
  using openxmb::video::color_matrix;
  const float kr=frame.matrix==color_matrix::bt709?0.2126F:frame.matrix==color_matrix::bt2020?0.2627F:0.299F;
  const float kb=frame.matrix==color_matrix::bt709?0.0722F:frame.matrix==color_matrix::bt2020?0.0593F:0.114F;
  return {kr,kb,frame.full_range?0.0F:16.0F,frame.full_range?1.0F/255:1.0F/219,
    frame.full_range?1.0F/255:1.0F/224,frame.format==openxmb::video::pixel_format::nv12?1u:0u,
    static_cast<std::uint32_t>(frame.width),static_cast<std::uint32_t>(frame.height)};
}
inline std::vector<std::uint8_t> rgba_fallback(const openxmb::video::frame& frame) {
  static_cast<void>(layout(frame));
  if(frame.format==openxmb::video::pixel_format::rgba) return frame.rgba;
  const auto p=parameters(frame);
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(frame.width)*frame.height*4);
  for(int y=0;y<frame.height;++y) for(int x=0;x<frame.width;++x) {
    const auto& u=frame.planes[1];const auto& v=frame.planes[2];
    const auto chroma=static_cast<std::size_t>(y/2)*u.stride+(x/2)*(p.nv12?2:1);
    const float Y=(frame.planes[0].bytes[static_cast<std::size_t>(y)*frame.planes[0].stride+x]-p.y_offset)*p.y_scale;
    const float U=(u.bytes[chroma]-128.0F)*p.chroma_scale;
    const float V=((p.nv12?u.bytes[chroma+1]:v.bytes[static_cast<std::size_t>(y/2)*v.stride+x/2])-128.0F)*p.chroma_scale;
    const float kg=1-p.kr-p.kb;
    const std::array values{Y+2*(1-p.kr)*V,Y-2*p.kb*(1-p.kb)/kg*U-2*p.kr*(1-p.kr)/kg*V,Y+2*(1-p.kb)*U};
    const auto index=(static_cast<std::size_t>(y)*frame.width+x)*4;
    for(int c=0;c<3;++c) rgba[index+c]=static_cast<std::uint8_t>(std::lround(std::clamp(values[c],0.0F,1.0F)*255));
    rgba[index+3]=255;
  }
  return rgba;
}
}
