#include "openxmb/integrations/xmb_overlay.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#ifdef __linux__
#include "x11_support.hpp"
#include <xcb/render.h>
#include <bit>
#include <thread>
#include <vector>
#endif
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
#elif defined(__linux__)
struct XmbOverlay::Impl {
  std::unique_ptr<x11_detail::Connection> x;
  OverlayBounds bounds{};OverlayCallback callback;OverlayStats stats{};
  std::thread::id owner{};xcb_window_t window{};xcb_colormap_t colormap{};xcb_gcontext_t gc{};
  std::vector<std::uint8_t> pixels;bool has_frame{};std::array<bool,256> held{};
  bool on_owner()const{return owner==std::this_thread::get_id();}
  void emit(OverlayInput input)noexcept{if(callback)try{callback(input);}catch(...) {}}
  bool upload(){
    const auto row=static_cast<std::size_t>(bounds.width)*4;const auto maximum=static_cast<std::size_t>(xcb_get_maximum_request_length(x->c))*4;if(maximum<64+row)return false;
    const auto rows=std::min<std::size_t>(65535,(maximum-64)/row);
    for(std::size_t y=0;y<bounds.height;y+=rows){const auto count=std::min<std::size_t>(rows,bounds.height-y);if(!x->check(xcb_put_image_checked(x->c,XCB_IMAGE_FORMAT_Z_PIXMAP,window,gc,bounds.width,count,0,y,0,32,count*row,pixels.data()+y*row)))return false;}
    xcb_flush(x->c);return true;
  }
  void pump(){
    if(!x||!window||!on_owner())return;
    for(unsigned count=0;count<256;++count){x11_detail::Reply<xcb_generic_event_t> event{xcb_poll_for_event(x->c)};if(!event)break;
      switch(event->response_type&0x7f){
      case XCB_EXPOSE:if(has_frame)(void)upload();break;
      case XCB_KEY_PRESS:case XCB_KEY_RELEASE:{const auto* key=reinterpret_cast<const xcb_key_press_event_t*>(event.get());const bool down=(event->response_type&0x7f)==XCB_KEY_PRESS;
        x11_detail::Reply<xcb_get_keyboard_mapping_reply_t> mapping{xcb_get_keyboard_mapping_reply(x->c,xcb_get_keyboard_mapping(x->c,key->detail,1),nullptr)};
        const auto symbol=mapping&&xcb_get_keyboard_mapping_keysyms_length(mapping.get())?xcb_get_keyboard_mapping_keysyms(mapping.get())[0]:0;
        const bool repeat=down&&held[key->detail];held[key->detail]=down;
        emit({down?OverlayInputKind::key_down:OverlayInputKind::key_up,symbol,key->detail,key->state,key->event_x,key->event_y,repeat});break;}
      case XCB_BUTTON_PRESS:case XCB_BUTTON_RELEASE:{const auto* button=reinterpret_cast<const xcb_button_press_event_t*>(event.get());emit({(event->response_type&0x7f)==XCB_BUTTON_PRESS?OverlayInputKind::mouse_down:OverlayInputKind::mouse_up,button->detail,0,button->state,button->event_x,button->event_y,false});break;}
      case XCB_MOTION_NOTIFY:{const auto* mouse=reinterpret_cast<const xcb_motion_notify_event_t*>(event.get());emit({OverlayInputKind::mouse_move,0,0,mouse->state,mouse->event_x,mouse->event_y,false});break;}
      case XCB_FOCUS_OUT:held.fill(false);emit({OverlayInputKind::focus_lost});break;
      case XCB_CLIENT_MESSAGE:{const auto* message=reinterpret_cast<const xcb_client_message_event_t*>(event.get());if(message->type==x->atom("WM_PROTOCOLS")&&message->data.data32[0]==x->atom("WM_DELETE_WINDOW"))emit({OverlayInputKind::close});break;}
      default:break;
      }
    }
  }
  bool mapped()const {if(!x||!window)return false;x11_detail::Reply<xcb_get_window_attributes_reply_t> reply{xcb_get_window_attributes_reply(x->c,xcb_get_window_attributes(x->c,window),nullptr)};return reply&&reply->map_state==XCB_MAP_STATE_VIEWABLE;}
  void release()noexcept{callback={};if(x){if(gc)xcb_free_gc(x->c,gc);if(window)xcb_destroy_window(x->c,window);if(colormap)xcb_free_colormap(x->c,colormap);xcb_flush(x->c);}x.reset();window=colormap=gc=0;pixels.clear();held.fill(false);has_frame=false;owner={};}
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
#elif defined(__linux__)
  if(impl_->window&&!impl_->on_owner())return {false,"The overlay must be prepared on its owning UI thread."};
  if(!x11_detail::x11_session())return {false,"Transparent overlays require a native X11 session; Wayland needs compositor-specific support."};
  const auto row=static_cast<std::size_t>(bounds.width)*4;
  if(!frame_bytes(bounds.width,bounds.height,row,frame_limit))return {false,"The overlay size is invalid or exceeds the supported memory limit."};
  impl_->release();impl_->x=std::make_unique<x11_detail::Connection>();auto& x=*impl_->x;
  if(!x||!x.compositor()){impl_->release();return {false,"An active X11 compositing manager is required for transparent overlays."};}
  const auto active=x.property(x.screen->root,"_NET_ACTIVE_WINDOW",XCB_ATOM_WINDOW,1);
  if(active.size()==1){const auto bypass=x.property(active[0],"_NET_WM_BYPASS_COMPOSITOR",XCB_ATOM_CARDINAL,1);if(!bypass.empty()&&bypass[0]==1){impl_->release();return {false,"The application requests compositor bypass; use the normal XMB window."};}}
  if(std::endian::native!=std::endian::little||xcb_get_setup(x.c)->image_byte_order!=XCB_IMAGE_ORDER_LSB_FIRST){impl_->release();return {false,"This X11 pixel byte order is unsupported."};}
  xcb_visualid_t visual{};
  x11_detail::Reply<xcb_render_query_pict_formats_reply_t> formats{xcb_render_query_pict_formats_reply(x.c,xcb_render_query_pict_formats(x.c),nullptr)};
  if(formats){
    auto screens=xcb_render_query_pict_formats_screens_iterator(formats.get());
    for(int screen=0;screen<x.screen_index&&screens.rem;++screen)xcb_render_pictscreen_next(&screens);
    if(screens.rem)for(auto depths=xcb_render_pictscreen_depths_iterator(screens.data);depths.rem;xcb_render_pictdepth_next(&depths))if(depths.data->depth==32)
      for(auto visuals=xcb_render_pictdepth_visuals_iterator(depths.data);visuals.rem;xcb_render_pictvisual_next(&visuals))
        for(auto f=xcb_render_query_pict_formats_formats_iterator(formats.get());f.rem;xcb_render_pictforminfo_next(&f))
          if(f.data->id==visuals.data->format&&f.data->direct.red_shift==16&&f.data->direct.green_shift==8&&f.data->direct.blue_shift==0&&f.data->direct.alpha_shift==24&&f.data->direct.red_mask==255&&f.data->direct.green_mask==255&&f.data->direct.blue_mask==255&&f.data->direct.alpha_mask==255)visual=visuals.data->visual;
  }
  if(!visual){impl_->release();return {false,"The X11 compositor has no supported ARGB visual."};}
  impl_->colormap=xcb_generate_id(x.c);impl_->window=xcb_generate_id(x.c);impl_->gc=xcb_generate_id(x.c);
  if(!x.check(xcb_create_colormap_checked(x.c,XCB_COLORMAP_ALLOC_NONE,impl_->colormap,x.screen->root,visual))){impl_->release();return {false,"X11 could not allocate the overlay colormap."};}
  const std::uint32_t values[]{0,0,XCB_EVENT_MASK_EXPOSURE|XCB_EVENT_MASK_KEY_PRESS|XCB_EVENT_MASK_KEY_RELEASE|XCB_EVENT_MASK_BUTTON_PRESS|XCB_EVENT_MASK_BUTTON_RELEASE|XCB_EVENT_MASK_POINTER_MOTION|XCB_EVENT_MASK_FOCUS_CHANGE,impl_->colormap};
  if(!x.check(xcb_create_window_checked(x.c,32,impl_->window,x.screen->root,bounds.x,bounds.y,bounds.width,bounds.height,0,XCB_WINDOW_CLASS_INPUT_OUTPUT,visual,XCB_CW_BACK_PIXEL|XCB_CW_BORDER_PIXEL|XCB_CW_EVENT_MASK|XCB_CW_COLORMAP,values))||!x.check(xcb_create_gc_checked(x.c,impl_->gc,impl_->window,0,nullptr))){impl_->release();return {false,"X11 could not create the ARGB overlay window."};}
  const std::uint32_t protocols[]{x.atom("WM_DELETE_WINDOW")},type[]{x.atom("_NET_WM_WINDOW_TYPE_UTILITY")},state[]{x.atom("_NET_WM_STATE_ABOVE")},pid[]{static_cast<std::uint32_t>(getpid())};
  if(!x.property(impl_->window,"WM_PROTOCOLS",XCB_ATOM_ATOM,protocols)||!x.property(impl_->window,"_NET_WM_WINDOW_TYPE",XCB_ATOM_ATOM,type)||!x.property(impl_->window,"_NET_WM_STATE",XCB_ATOM_ATOM,state)||!x.property(impl_->window,"_NET_WM_PID",XCB_ATOM_CARDINAL,pid)){impl_->release();return {false,"X11 could not configure the overlay window."};}
  impl_->bounds=bounds;impl_->callback=std::move(callback);impl_->owner=std::this_thread::get_id();impl_->stats={};impl_->pixels.resize(row*bounds.height);(void)conversion_table();return {true,{}};
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
  if(IsWindowVisible(impl_->window)&&!SetWindowPos(impl_->window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE))
    return {false,"Windows could not maintain the overlay above the application."};
  const auto end=std::chrono::steady_clock::now();++impl_->stats.frames;impl_->stats.conversion_ns+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(ready-begin).count());impl_->stats.present_ns+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end-ready).count());impl_->has_frame=true;return {true,{}};
#elif defined(__linux__)
  if(!impl_->window||!impl_->on_owner())return {false,"The overlay is not prepared on this UI thread."};
  auto& x=*impl_->x;impl_->pump();
  if(!x.compositor())return {false,"The X11 compositing manager is no longer available."};
  if(width!=impl_->bounds.width||height!=impl_->bounds.height)return {false,"The overlay frame dimensions do not match the prepared window."};
  const auto begin=std::chrono::steady_clock::now();const auto row=static_cast<std::size_t>(width)*4;
  const auto converted=convert_overlay_bgra(bgra,width,height,stride,impl_->pixels,row);if(!converted)return converted;
  const auto ready=std::chrono::steady_clock::now();if(!impl_->upload())return {false,"X11 could not present the transparent overlay."};
  xcb_flush(x.c);const auto end=std::chrono::steady_clock::now();++impl_->stats.frames;impl_->stats.conversion_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(ready-begin).count();impl_->stats.present_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(end-ready).count();impl_->has_frame=true;return {true,{}};
#else
  (void)bgra;(void)width;(void)height;(void)stride;return {false,"Transparent overlay is unavailable on this platform."};
#endif
}
Result XmbOverlay::show(bool activate) {
#ifdef _WIN32
  if(!impl_->window||!impl_->on_owner())return {false,"The overlay is not prepared on this UI thread."};
  if(!impl_->has_frame)return {false,"The overlay needs a rendered frame before it can be shown."};
  if(!SetWindowPos(impl_->window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW)||!IsWindowVisible(impl_->window))return {false,"Windows did not show the overlay."};
  if(activate&&(!SetForegroundWindow(impl_->window)||GetForegroundWindow()!=impl_->window)){ShowWindow(impl_->window,SW_HIDE);return {false,"Windows did not grant overlay focus. Switch to OpenXMB manually."};}
  return {true,{}};
#elif defined(__linux__)
  if(!impl_->window||!impl_->on_owner())return {false,"The overlay is not prepared on this UI thread."};
  if(!impl_->has_frame)return {false,"The overlay needs a rendered frame before it can be shown."};
  auto& x=*impl_->x;if(!x.compositor()||!x.check(xcb_map_window_checked(x.c,impl_->window)))return {false,"The X11 compositor could not show the overlay."};
  if(activate&&!x.activate(impl_->window)){hide();return {false,"The window manager could not request overlay focus."};}
  for(unsigned tries=0;tries<20;++tries){impl_->pump();x11_detail::Reply<xcb_get_input_focus_reply_t> focus{xcb_get_input_focus_reply(x.c,xcb_get_input_focus(x.c),nullptr)};if(impl_->mapped()&&(!activate||(focus&&focus->focus==impl_->window&&x.active(impl_->window)))){if(impl_->upload())return {true,{}};hide();return {false,"The X11 overlay frame could not be restored after mapping."};}std::this_thread::sleep_for(std::chrono::milliseconds(5));}
  hide();return {false,"The X11 window manager did not grant overlay focus. Switch to OpenXMB manually."};
#else
  (void)activate;return {false,"Transparent overlay is unavailable on this platform."};
#endif
}
void XmbOverlay::hide(){
#ifdef _WIN32
  if(impl_->window&&impl_->on_owner())ShowWindow(impl_->window,SW_HIDE);
#elif defined(__linux__)
  if(impl_->window&&impl_->on_owner()){xcb_unmap_window(impl_->x->c,impl_->window);xcb_flush(impl_->x->c);impl_->held.fill(false);}
#endif
}
bool XmbOverlay::visible()const{
#ifdef _WIN32
  return impl_->window&&IsWindowVisible(impl_->window);
#elif defined(__linux__)
  if(!impl_->on_owner())return false;impl_->pump();return impl_->mapped();
#else
  return false;
#endif
}
bool XmbOverlay::focused()const{
#ifdef _WIN32
  return impl_->window&&GetForegroundWindow()==impl_->window;
#elif defined(__linux__)
  if(!impl_->window||!impl_->on_owner())return false;impl_->pump();x11_detail::Reply<xcb_get_input_focus_reply_t> focus{xcb_get_input_focus_reply(impl_->x->c,xcb_get_input_focus(impl_->x->c),nullptr)};return impl_->mapped()&&focus&&focus->focus==impl_->window;
#else
  return false;
#endif
}
std::uintptr_t XmbOverlay::native_handle()const{
#ifdef _WIN32
  return reinterpret_cast<std::uintptr_t>(impl_->window);
#elif defined(__linux__)
  return impl_->window;
#else
  return 0;
#endif
}
OverlayStats XmbOverlay::stats()const{return impl_->stats;}
} // namespace openxmb::integrations
