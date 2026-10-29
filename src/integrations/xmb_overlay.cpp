#include "openxmb/integrations/xmb_overlay.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <mutex>
#endif
namespace openxmb::integrations {
namespace {
constexpr unsigned dimension_limit=16384;
constexpr std::size_t frame_limit=256u*1024u*1024u;
using Lut=std::array<std::array<std::uint8_t,256>,256>;
const Lut& conversion_table() {
  static const Lut value=[] {
    Lut out{};
    for(unsigned alpha=1;alpha<256;++alpha) for(unsigned channel=0;channel<256;++channel) {
      const double encoded=channel/255.0;
      const double linear=encoded<=0.04045?encoded/12.92:std::pow((encoded+0.055)/1.055,2.4);
      const double straight=std::min(1.0,linear*255.0/alpha);
      const double srgb=straight<=0.0031308?straight*12.92:1.055*std::pow(straight,1.0/2.4)-0.055;
      out[alpha][channel]=static_cast<std::uint8_t>(std::clamp(std::lround(srgb*alpha),0l,255l));
    }
    return out;
  }();return value;
}
bool frame_bytes(unsigned width,unsigned height,std::size_t stride,std::size_t available) {
  if(!width||!height||width>dimension_limit||height>dimension_limit)return false;
  const auto row=static_cast<std::size_t>(width)*4;if(row>frame_limit||stride<row)return false;
  if(static_cast<std::size_t>(height)>frame_limit/row)return false;
  const auto rows=static_cast<std::size_t>(height)-1;
  if(rows && stride>(std::numeric_limits<std::size_t>::max()-row)/rows)return false;
  return rows*stride+row<=available;
}
bool overlaps(std::span<const std::uint8_t> a,std::span<std::uint8_t> b) {
  if(a.empty()||b.empty())return false;
  const auto x=reinterpret_cast<std::uintptr_t>(a.data()),y=reinterpret_cast<std::uintptr_t>(b.data());
  return x<=y?y-x<a.size():x-y<b.size();
}
} // namespace
Result convert_overlay_bgra(std::span<const std::uint8_t> input,unsigned width,unsigned height,std::size_t input_stride,
                            std::span<std::uint8_t> output,std::size_t output_stride) {
  if(!frame_bytes(width,height,input_stride,input.size())||!frame_bytes(width,height,output_stride,output.size())||overlaps(input,output))
    return {false,"Invalid overlay frame dimensions, stride, buffer, or overlapping storage."};
  const auto& table=conversion_table();
  for(unsigned y=0;y<height;++y){const auto* src=input.data()+static_cast<std::size_t>(y)*input_stride;auto* dst=output.data()+static_cast<std::size_t>(y)*output_stride;
    for(unsigned x=0;x<width;++x){const auto at=static_cast<std::size_t>(x)*4;const auto a=src[at+3];dst[at]=table[a][src[at]];dst[at+1]=table[a][src[at+1]];dst[at+2]=table[a][src[at+2]];dst[at+3]=a;}}
  return {true,{}};
}
#ifdef _WIN32
struct XmbOverlay::Impl {
  OverlayBounds bounds{};OverlayCallback callback;OverlayStats stats{};DWORD owner{};
  HWND window{};HDC dc{};HBITMAP bitmap{};HGDIOBJ previous{};void* pixels{};bool has_frame{};
  static unsigned modifiers() {
    unsigned value{};if(GetKeyState(VK_SHIFT)&0x8000)value|=MOD_SHIFT;if(GetKeyState(VK_CONTROL)&0x8000)value|=MOD_CONTROL;
    if(GetKeyState(VK_MENU)&0x8000)value|=MOD_ALT;if((GetKeyState(VK_LWIN)|GetKeyState(VK_RWIN))&0x8000)value|=MOD_WIN;return value;
  }
  void emit(OverlayInput input) noexcept {if(callback)try{callback(input);}catch(...) {}}
  static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(message==WM_NCCREATE){auto* info=reinterpret_cast<CREATESTRUCTW*>(lp);self=static_cast<Impl*>(info->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(!self)return DefWindowProcW(hwnd,message,wp,lp);
    switch(message){
    case WM_CLOSE:self->emit({OverlayInputKind::close});return 0;
    case WM_KILLFOCUS:self->emit({OverlayInputKind::focus_lost});return 0;
    case WM_SYSKEYDOWN:case WM_KEYDOWN:case WM_SYSKEYUP:case WM_KEYUP: {
      const bool down=message==WM_KEYDOWN||message==WM_SYSKEYDOWN;
      if(down&&wp==VK_F4&&(lp&(1LL<<29))){self->emit({OverlayInputKind::close});return 0;}
      const auto scan=static_cast<unsigned>((static_cast<std::uintptr_t>(lp)>>16)&0xff)|((lp&(1LL<<24))?0x100u:0u);
      self->emit({down?OverlayInputKind::key_down:OverlayInputKind::key_up,static_cast<unsigned>(wp),scan,modifiers(),0,0,down&&(lp&(1LL<<30))!=0});return 0;
    }
    case WM_MOUSEMOVE:self->emit({OverlayInputKind::mouse_move,0,0,modifiers(),GET_X_LPARAM(lp),GET_Y_LPARAM(lp),false});return 0;
    case WM_LBUTTONDOWN:case WM_RBUTTONDOWN:case WM_MBUTTONDOWN:case WM_LBUTTONUP:case WM_RBUTTONUP:case WM_MBUTTONUP:{
      const bool down=message==WM_LBUTTONDOWN||message==WM_RBUTTONDOWN||message==WM_MBUTTONDOWN;
      const unsigned key=(message==WM_LBUTTONDOWN||message==WM_LBUTTONUP)?VK_LBUTTON:(message==WM_RBUTTONDOWN||message==WM_RBUTTONUP)?VK_RBUTTON:VK_MBUTTON;
      self->emit({down?OverlayInputKind::mouse_down:OverlayInputKind::mouse_up,key,0,modifiers(),GET_X_LPARAM(lp),GET_Y_LPARAM(lp),false});return 0;
    }
    case WM_SYSCHAR:return 0;
    case WM_NCDESTROY:SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);break;
    }return DefWindowProcW(hwnd,message,wp,lp);
  }
  static bool register_class() {
    static std::once_flag once;static bool registered{};
    std::call_once(once,[]{WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=procedure;cls.lpszClassName=L"OpenXMB.LayeredOverlay.v1";cls.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
      if(RegisterClassExW(&cls))registered=true;else if(GetLastError()==ERROR_CLASS_ALREADY_EXISTS){WNDCLASSEXW existing{};existing.cbSize=sizeof(existing);registered=GetClassInfoExW(cls.hInstance,cls.lpszClassName,&existing)&&existing.lpfnWndProc==procedure;}});return registered;
  }
  bool on_owner()const{return owner&&GetCurrentThreadId()==owner;}
  void release() noexcept {
    if(window){SetWindowLongPtrW(window,GWLP_USERDATA,0);DestroyWindow(window);window=nullptr;}
    if(dc&&previous&&previous!=HGDI_ERROR)SelectObject(dc,previous);previous=nullptr;
    if(bitmap)DeleteObject(bitmap);bitmap=nullptr;pixels=nullptr;if(dc)DeleteDC(dc);dc=nullptr;has_frame=false;callback={};owner=0;
  }
  ~Impl(){release();}
};
#else
struct XmbOverlay::Impl { OverlayStats stats{}; };
#endif
XmbOverlay::XmbOverlay():impl_(std::make_unique<Impl>()){}
XmbOverlay::~XmbOverlay()=default;
Result XmbOverlay::prepare(OverlayBounds bounds,OverlayCallback callback) {
#ifdef _WIN32
  if(impl_->window&&!impl_->on_owner())return {false,"The overlay must be prepared on its owning UI thread."};
  const auto row=static_cast<std::size_t>(bounds.width)*4;
  if(!frame_bytes(bounds.width,bounds.height,row,frame_limit))return {false,"The overlay size is invalid or exceeds the supported memory limit."};
  impl_->release();impl_->owner=GetCurrentThreadId();impl_->bounds=bounds;impl_->callback=std::move(callback);impl_->stats={};
  if(!Impl::register_class()){impl_->release();return {false,"Windows could not register the overlay window."};}
  impl_->window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TOPMOST,L"OpenXMB.LayeredOverlay.v1",L"OpenXMB",WS_POPUP,
    bounds.x,bounds.y,static_cast<int>(bounds.width),static_cast<int>(bounds.height),nullptr,nullptr,GetModuleHandleW(nullptr),impl_.get());
  if(!impl_->window){impl_->release();return {false,"Windows could not create the overlay window."};}
  impl_->dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=static_cast<LONG>(bounds.width);info.bmiHeader.biHeight=-static_cast<LONG>(bounds.height);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
  if(impl_->dc)impl_->bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&impl_->pixels,nullptr,0);
  if(impl_->dc&&impl_->bitmap)impl_->previous=SelectObject(impl_->dc,impl_->bitmap);
  if(!impl_->dc||!impl_->bitmap||!impl_->pixels||!impl_->previous||impl_->previous==HGDI_ERROR){impl_->release();return {false,"Windows could not allocate the overlay frame buffer."};}
  (void)conversion_table();return {true,{}};
#else
  (void)bounds;(void)callback;return {false,"Transparent overlay is unavailable on this platform."};
#endif
}
Result XmbOverlay::present(std::span<const std::uint8_t> bgra,unsigned width,unsigned height,std::size_t stride) {
#ifdef _WIN32
  if(!impl_->window||!impl_->on_owner())return {false,"The overlay is not prepared on this UI thread."};
  if(width!=impl_->bounds.width||height!=impl_->bounds.height)return {false,"The overlay frame dimensions do not match the prepared window."};
  const auto begin=std::chrono::steady_clock::now();const auto output_size=static_cast<std::size_t>(width)*height*4;
  const auto converted=convert_overlay_bgra(bgra,width,height,stride,{static_cast<std::uint8_t*>(impl_->pixels),output_size},static_cast<std::size_t>(width)*4);
  if(!converted)return converted;
  const auto ready=std::chrono::steady_clock::now();POINT position{impl_->bounds.x,impl_->bounds.y},source{0,0};SIZE size{static_cast<LONG>(width),static_cast<LONG>(height)};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
  if(!UpdateLayeredWindow(impl_->window,nullptr,&position,&size,impl_->dc,&source,0,&blend,ULW_ALPHA))return {false,"Windows could not present the transparent overlay."};
  const auto end=std::chrono::steady_clock::now();++impl_->stats.frames;impl_->stats.conversion_ns+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(ready-begin).count());impl_->stats.present_ns+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end-ready).count());impl_->has_frame=true;return {true,{}};
#else
  (void)bgra;(void)width;(void)height;(void)stride;return {false,"Transparent overlay is unavailable on this platform."};
#endif
}
Result XmbOverlay::show(bool activate) {
#ifdef _WIN32
  if(!impl_->window||!impl_->on_owner())return {false,"The overlay is not prepared on this UI thread."};
  if(!impl_->has_frame)return {false,"The overlay needs a rendered frame before it can be shown."};
  ShowWindow(impl_->window,SW_SHOWNOACTIVATE);if(!IsWindowVisible(impl_->window))return {false,"Windows did not show the overlay."};
  if(activate&&(!SetForegroundWindow(impl_->window)||GetForegroundWindow()!=impl_->window)){ShowWindow(impl_->window,SW_HIDE);return {false,"Windows did not grant overlay focus. Switch to OpenXMB manually."};}
  return {true,{}};
#else
  (void)activate;return {false,"Transparent overlay is unavailable on this platform."};
#endif
}
void XmbOverlay::hide(){
#ifdef _WIN32
  if(impl_->window&&impl_->on_owner())ShowWindow(impl_->window,SW_HIDE);
#endif
}
bool XmbOverlay::visible()const{
#ifdef _WIN32
  return impl_->window&&IsWindowVisible(impl_->window);
#else
  return false;
#endif
}
bool XmbOverlay::focused()const{
#ifdef _WIN32
  return impl_->window&&GetForegroundWindow()==impl_->window;
#else
  return false;
#endif
}
std::uintptr_t XmbOverlay::native_handle()const{
#ifdef _WIN32
  return reinterpret_cast<std::uintptr_t>(impl_->window);
#else
  return 0;
#endif
}
OverlayStats XmbOverlay::stats()const{return impl_->stats;}
} // namespace openxmb::integrations
