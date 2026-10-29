#include "openxmb/integrations/xmb_overlay.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#ifdef __linux__
#include "x11_support.hpp"
#include <xcb/render.h>
#include <xcb/xcbext.h>
#include <X11/extensions/XKBproto.h>
#include <X11/extensions/shapeconst.h>
#include <bit>
#include <thread>
#include <vector>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "focus_handoff.hpp"
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
#if defined(_WIN32)||defined(__linux__)
std::string size_mismatch(unsigned width,unsigned height,const OverlayBounds& bounds) {
  return "The overlay frame is "+std::to_string(width)+"x"+std::to_string(height)+" but the host is prepared for "+
    std::to_string(bounds.width)+"x"+std::to_string(bounds.height)+".";
}
#endif
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
namespace {
using focus_handoff::windows_error;
Result windows_failure(std::string_view action) {const DWORD code=GetLastError();return {false,windows_error(action,code)};}
std::string hwnd_text(HWND window) {return focus_handoff::hwnd_text(reinterpret_cast<std::uintptr_t>(window));}
// SetForegroundWindow documents no extended error (observed: a stale
// ERROR_FILE_NOT_FOUND after SetLastError(0)), so only its BOOL is reported.
std::string bool_call(const char* call,BOOL value) {return std::string(call)+(value?"=TRUE":"=FALSE");}
struct ForegroundHolder {HWND window{};DWORD thread{},pid{};focus_handoff::ProcessFacts facts;};
ForegroundHolder foreground_holder() {
  ForegroundHolder holder;holder.window=GetForegroundWindow();
  if(holder.window)holder.thread=GetWindowThreadProcessId(holder.window,&holder.pid);
  if(holder.thread)holder.facts=focus_handoff::process_facts(holder.pid);
  return holder;
}
std::string describe_holder(const ForegroundHolder& holder) {
  const std::filesystem::path image{holder.facts.image};
  const auto process=image.empty()?"process "+std::to_string(holder.pid):focus_handoff::narrow(image.filename().native())+" (process "+std::to_string(holder.pid)+")";
  return process+" window "+hwnd_text(holder.window);
}
// The source must be the validated session window or a window it owns; other
// windows of the same process never authorise a handoff.
bool is_session(const ForegroundHolder& holder,const SessionWindowTarget& session) {
  return holder.thread&&static_cast<std::int64_t>(holder.pid)==session.pid&&holder.facts.creation&&holder.facts.creation==session.process_creation&&
    focus_handoff::session_window_or_owned(holder.window,reinterpret_cast<HWND>(session.handle));
}
// SetForegroundWindow admits a background process only when it received the
// last input event (Microsoft Learn, SetForegroundWindow remarks). A Guide
// press reaches OpenXMB through GameInput/SDL, never as window input, so plain
// activation is refused while a game owns the foreground. The authorised
// handoff runs in an isolated helper process (focus_handoff.hpp): this UI
// thread never attaches its input queue and waits at most
// focus_handoff::deadline while dispatching its own messages.
Result activate_window(HWND window,const std::optional<SessionWindowTarget>& session) {
  const HWND before=GetForegroundWindow();
  const BOOL plain=SetForegroundWindow(window);
  const auto holder=foreground_holder();
  if(holder.window==window)return {true,"Overlay focus: plain SetForegroundWindow admitted the request."};
  const std::string refused="Windows refused overlay focus ["+bool_call("SetForegroundWindow",plain)+
    ", foreground "+hwnd_text(before)+" -> "+hwnd_text(holder.window)+"]";
  // TRUE means Windows admitted the request; a different foreground is then a
  // race with another activation, not a denial, so no handoff is attempted.
  if(plain)return {false,refused+": Windows admitted the request but another window holds the foreground."};
  if(!holder.thread)return {false,refused+": no foreground window exists to hand off from."};
  const auto name=describe_holder(holder);
  if(!session)return {false,refused+": "+name+" owns the foreground and OpenXMB did not receive the last input event; no handoff was authorised."};
  if(!is_session(holder,*session)||session->pid<=0||session->pid>std::numeric_limits<DWORD>::max())
    return {false,refused+": "+name+" is not the validated session window "+hwnd_text(reinterpret_cast<HWND>(session->handle))+" or a window it owns; no handoff was attempted."};
  // Cheap local refusal; the helper repeats every check with its preflight.
  if(IsHungAppWindow(holder.window))return {false,refused+": "+name+" is not responding; no handoff was attempted."};
  const auto own=focus_handoff::process_facts(GetCurrentProcessId());
  if(!own.creation)return {false,refused+": OpenXMB could not read its own process identity; no handoff was attempted."};
  const focus_handoff::Request request{GetCurrentProcessId(),own.creation,reinterpret_cast<std::uintptr_t>(window),GetCurrentThreadId(),
    static_cast<DWORD>(session->pid),session->process_creation,session->handle};
  auto handed=focus_handoff::hand_off(window,request,focus_handoff::sibling_helper(),std::chrono::steady_clock::now()+focus_handoff::deadline);
  if(!handed)handed.message=refused+": "+handed.message;
  return handed;
}
std::optional<RECT> monitor_rect(HMONITOR monitor) {
  MONITORINFO info{};info.cbSize=sizeof(info);
  if(!monitor||!GetMonitorInfoW(monitor,&info))return std::nullopt;
  return info.rcMonitor;
}
RECT monitor_of(OverlayBounds bounds) {
  const RECT area{bounds.x,bounds.y,bounds.x+static_cast<LONG>(bounds.width),bounds.y+static_cast<LONG>(bounds.height)};
  return monitor_rect(MonitorFromRect(&area,MONITOR_DEFAULTTONEAREST)).value_or(RECT{});
}
// A 32-bit top-down DIB selected into a memory DC; UpdateLayeredWindow's source.
struct Surface {
  HDC dc{};HBITMAP bitmap{};HGDIOBJ previous{};void* pixels{};
  void release() noexcept {
    if(dc&&previous&&previous!=HGDI_ERROR)SelectObject(dc,previous);
    if(bitmap)DeleteObject(bitmap);
    if(dc)DeleteDC(dc);
    *this={};
  }
};
// Replaces `out` only after every allocation succeeded.
Result make_surface(unsigned width,unsigned height,Surface& out) {
  Surface next;next.dc=CreateCompatibleDC(nullptr);
  if(!next.dc)return windows_failure("Windows could not create the overlay device context");
  BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=static_cast<LONG>(width);
  info.bmiHeader.biHeight=-static_cast<LONG>(height);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
  next.bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&next.pixels,nullptr,0);
  if(!next.bitmap||!next.pixels){auto failure=windows_failure("Windows could not allocate the overlay frame buffer");next.release();return failure;}
  next.previous=SelectObject(next.dc,next.bitmap);
  if(!next.previous||next.previous==HGDI_ERROR){auto failure=windows_failure("Windows could not select the overlay frame buffer");next.release();return failure;}
  out.release();out=next;return {true,{}};
}
} // namespace
struct XmbOverlay::Impl {
  OverlayBounds bounds{};RECT monitor{};OverlayCallback callback;OverlayStats stats{};DWORD owner{};
  HWND window{};Surface surface;bool has_frame{},passive{};
  static unsigned modifiers() {
    unsigned value{};if(GetKeyState(VK_SHIFT)&0x8000)value|=MOD_SHIFT;if(GetKeyState(VK_CONTROL)&0x8000)value|=MOD_CONTROL;
    if(GetKeyState(VK_MENU)&0x8000)value|=MOD_ALT;if((GetKeyState(VK_LWIN)|GetKeyState(VK_RWIN))&0x8000)value|=MOD_WIN;return value;
  }
  void emit(OverlayInput input) noexcept {if(callback)try{callback(input);}catch(...) {}}
  static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(message==WM_NCCREATE){auto* info=reinterpret_cast<CREATESTRUCTW*>(lp);self=static_cast<Impl*>(info->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(!self)return DefWindowProcW(hwnd,message,wp,lp);
    if(self->passive) {
      // Hit-test transparency complements WS_EX_TRANSPARENT; the passive host
      // never routes input, focus loss or close requests to the application.
      switch(message){
      case WM_NCHITTEST:return HTTRANSPARENT;
      case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
      case WM_CLOSE:return 0;
      case WM_NCDESTROY:SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);break;
      default:break;
      }return DefWindowProcW(hwnd,message,wp,lp);
    }
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
  static Result register_class() {
    static std::once_flag once;static DWORD error{};static bool registered{};
    std::call_once(once,[]{WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=procedure;cls.lpszClassName=focus_handoff::overlay_class;cls.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
      if(RegisterClassExW(&cls)){registered=true;return;}
      error=GetLastError();
      if(error==ERROR_CLASS_ALREADY_EXISTS){WNDCLASSEXW existing{};existing.cbSize=sizeof(existing);registered=GetClassInfoExW(cls.hInstance,cls.lpszClassName,&existing)&&existing.lpfnWndProc==procedure;}});
    if(registered)return {true,{}};
    return {false,windows_error("Windows could not register the overlay window class",error)};
  }
  bool on_owner()const{return owner&&GetCurrentThreadId()==owner;}
  void release() noexcept {
    if(window){SetWindowLongPtrW(window,GWLP_USERDATA,0);DestroyWindow(window);window=nullptr;}
    surface.release();has_frame=false;passive=false;callback={};owner=0;
  }
  ~Impl(){release();}
};
#elif defined(__linux__)
namespace {
xcb_extension_t overlay_xkb_extension{"XKEYBOARD",0};
xcb_extension_t overlay_shape_extension{SHAPENAME,0};
// Raw requests keep the existing libxcb link set; these extensions have no
// separately linked XCB binding here.
template<class Reply,class Request>
x11_detail::Reply<Reply> overlay_request(x11_detail::Connection& x,xcb_extension_t& extension,unsigned opcode,Request request) {
  const xcb_protocol_request_t protocol{2,&extension,static_cast<std::uint8_t>(opcode),0};
  iovec parts[4]{};parts[2].iov_base=&request;parts[2].iov_len=sizeof(request);parts[3].iov_len=(-sizeof(request))&3;
  const auto sequence=xcb_send_request(x.c,XCB_REQUEST_CHECKED,parts+2,&protocol);if(!sequence)return {};
  xcb_generic_error_t* error{};x11_detail::Reply<Reply> reply{static_cast<Reply*>(xcb_wait_for_reply(x.c,sequence,&error))};
  x11_detail::Reply<xcb_generic_error_t> release(error);return error?x11_detail::Reply<Reply>{}:std::move(reply);
}
bool overlay_detectable_repeat(x11_detail::Connection& x) {
  // XKB repeat mode is per connection: the hotkey's separate XCB connection
  // cannot prevent synthetic release/press pairs in this overlay event pump.
  const auto* extension=xcb_get_extension_data(x.c,&overlay_xkb_extension);if(!extension||!extension->present)return false;
  xkbUseExtensionReq use{};use.wantedMajor=1;use.wantedMinor=0;
  const auto version=overlay_request<xkbUseExtensionReply>(x,overlay_xkb_extension,X_kbUseExtension,use);if(!version||!version->supported)return false;
  xkbPerClientFlagsReq request{};request.deviceSpec=XkbUseCoreKbd;request.change=XkbPCF_DetectableAutoRepeatMask;request.value=XkbPCF_DetectableAutoRepeatMask;
  const auto flags=overlay_request<xkbPerClientFlagsReply>(x,overlay_xkb_extension,X_kbPerClientFlags,request);
  return flags&&(flags->supported&XkbPCF_DetectableAutoRepeatMask)&&(flags->value&XkbPCF_DetectableAutoRepeatMask);
}
// SHAPE protocol wire layouts (shapeproto.h, with 32-bit XIDs): QueryVersion
// is minor opcode 0 and Rectangles is 1. Input shapes require SHAPE 1.1.
struct ShapeVersionRequest {std::uint8_t major{},minor{};std::uint16_t length{};};
struct ShapeVersionReply {std::uint8_t type{},unused{};std::uint16_t sequence{};std::uint32_t length{};std::uint16_t major{},minor{};std::uint8_t pad[20]{};};
struct ShapeRectanglesRequest {std::uint8_t major{},minor{};std::uint16_t length{};std::uint8_t op{},kind{},ordering{},pad{};std::uint32_t window{};std::int16_t x{},y{};};
static_assert(sizeof(ShapeVersionRequest)==4&&sizeof(ShapeVersionReply)==32&&sizeof(ShapeRectanglesRequest)==16);
bool overlay_empty_input(x11_detail::Connection& x,xcb_window_t window) {
  // Setting zero Input rectangles leaves the window visible but with no
  // pointer/touch region, so events reach whatever is below it.
  const auto* extension=xcb_get_extension_data(x.c,&overlay_shape_extension);if(!extension||!extension->present)return false;
  const auto version=overlay_request<ShapeVersionReply>(x,overlay_shape_extension,0,ShapeVersionRequest{});
  if(!version||version->major<1||(version->major==1&&version->minor<1))return false;
  ShapeRectanglesRequest request{};request.op=ShapeSet;request.kind=ShapeInput;request.window=window;
  const xcb_protocol_request_t protocol{2,&overlay_shape_extension,1,1};
  iovec parts[4]{};parts[2].iov_base=&request;parts[2].iov_len=sizeof(request);
  const auto sequence=xcb_send_request(x.c,XCB_REQUEST_CHECKED,parts+2,&protocol);
  return sequence&&x.check(xcb_void_cookie_t{sequence});
}
// Empty on success; otherwise the X11 error code and failing request opcodes.
std::string x11_failure(x11_detail::Connection& x,xcb_void_cookie_t cookie) {
  x11_detail::Reply<xcb_generic_error_t> error{xcb_request_check(x.c,cookie)};
  if(error)return " (X11 error "+std::to_string(error->error_code)+", request "+std::to_string(error->major_code)+"."+std::to_string(error->minor_code)+")";
  if(const int code=xcb_connection_has_error(x.c))return " (XCB connection error "+std::to_string(code)+")";
  return {};
}
}
struct XmbOverlay::Impl {
  std::unique_ptr<x11_detail::Connection> x;
  OverlayBounds bounds{};OverlayCallback callback;OverlayStats stats{};
  std::thread::id owner{};xcb_window_t window{};xcb_colormap_t colormap{};xcb_gcontext_t gc{};
  std::vector<std::uint8_t> pixels;bool has_frame{},passive{},display_dirty{};std::array<bool,256> held{};
  std::uint16_t root_width{},root_height{};
  bool on_owner()const{return owner==std::this_thread::get_id();}
  void emit(OverlayInput input)noexcept{if(callback)try{callback(input);}catch(...) {}}
  // Empty on success.
  std::string upload(){
    const auto row=static_cast<std::size_t>(bounds.width)*4;const auto maximum=static_cast<std::size_t>(xcb_get_maximum_request_length(x->c))*4;
    if(maximum<64+row)return " (one "+std::to_string(row)+"-byte row exceeds the "+std::to_string(maximum)+"-byte X11 request limit)";
    const auto rows=std::min<std::size_t>(65535,(maximum-64)/row);
    for(std::size_t y=0;y<bounds.height;y+=rows){const auto count=std::min<std::size_t>(rows,bounds.height-y);
      if(auto failure=x11_failure(*x,xcb_put_image_checked(x->c,XCB_IMAGE_FORMAT_Z_PIXMAP,window,gc,bounds.width,count,0,y,0,32,count*row,pixels.data()+y*row));!failure.empty())return failure;}
    xcb_flush(x->c);return {};
  }
  void pump(){
    if(!x||!window||!on_owner())return;
    for(unsigned count=0;count<256;++count){x11_detail::Reply<xcb_generic_event_t> event{xcb_poll_for_event(x->c)};if(!event)break;
      const auto type=event->response_type&0x7f;
      if(type==XCB_EXPOSE){if(has_frame)(void)upload();continue;}
      // Root StructureNotify: RandR mode, scale or layout changes resize the root.
      if(type==XCB_CONFIGURE_NOTIFY){const auto* configure=reinterpret_cast<const xcb_configure_notify_event_t*>(event.get());
        if(configure->window==x->screen->root&&(configure->width!=root_width||configure->height!=root_height)){root_width=configure->width;root_height=configure->height;display_dirty=true;}
        continue;}
      // A passive host selects only Expose; nothing else may become input.
      if(passive)continue;
      switch(type){
      case XCB_KEY_PRESS:case XCB_KEY_RELEASE:{const auto* key=reinterpret_cast<const xcb_key_press_event_t*>(event.get());const bool down=type==XCB_KEY_PRESS;
        x11_detail::Reply<xcb_get_keyboard_mapping_reply_t> mapping{xcb_get_keyboard_mapping_reply(x->c,xcb_get_keyboard_mapping(x->c,key->detail,1),nullptr)};
        const auto symbol=mapping&&xcb_get_keyboard_mapping_keysyms_length(mapping.get())?xcb_get_keyboard_mapping_keysyms(mapping.get())[0]:0;
        const bool repeat=down&&held[key->detail];held[key->detail]=down;
        emit({down?OverlayInputKind::key_down:OverlayInputKind::key_up,symbol,key->detail,key->state,key->event_x,key->event_y,repeat});break;}
      case XCB_BUTTON_PRESS:case XCB_BUTTON_RELEASE:{const auto* button=reinterpret_cast<const xcb_button_press_event_t*>(event.get());emit({type==XCB_BUTTON_PRESS?OverlayInputKind::mouse_down:OverlayInputKind::mouse_up,button->detail,0,button->state,button->event_x,button->event_y,false});break;}
      case XCB_MOTION_NOTIFY:{const auto* mouse=reinterpret_cast<const xcb_motion_notify_event_t*>(event.get());emit({OverlayInputKind::mouse_move,0,0,mouse->state,mouse->event_x,mouse->event_y,false});break;}
      case XCB_FOCUS_OUT:held.fill(false);emit({OverlayInputKind::focus_lost});break;
      case XCB_CLIENT_MESSAGE:{const auto* message=reinterpret_cast<const xcb_client_message_event_t*>(event.get());if(message->type==x->atom("WM_PROTOCOLS")&&message->data.data32[0]==x->atom("WM_DELETE_WINDOW"))emit({OverlayInputKind::close});break;}
      default:break;
      }
    }
  }
  bool mapped()const {if(!x||!window)return false;x11_detail::Reply<xcb_get_window_attributes_reply_t> reply{xcb_get_window_attributes_reply(x->c,xcb_get_window_attributes(x->c,window),nullptr)};return reply&&reply->map_state==XCB_MAP_STATE_VIEWABLE;}
  // Passive hosts can be covered when the window manager raises a managed
  // game frame; restacking an override-redirect window never moves focus.
  void raise()const{const std::uint32_t above[]{XCB_STACK_MODE_ABOVE};xcb_configure_window(x->c,window,XCB_CONFIG_WINDOW_STACK_MODE,above);}
  void release()noexcept{callback={};if(x){if(gc)xcb_free_gc(x->c,gc);if(window)xcb_destroy_window(x->c,window);if(colormap)xcb_free_colormap(x->c,colormap);xcb_flush(x->c);}x.reset();window=colormap=gc=0;pixels.clear();held.fill(false);has_frame=false;passive=false;display_dirty=false;root_width=root_height=0;owner={};}
  ~Impl(){release();}
};
#else
struct XmbOverlay::Impl { OverlayStats stats{}; };
#endif
XmbOverlay::XmbOverlay():impl_(std::make_unique<Impl>()){}
XmbOverlay::~XmbOverlay()=default;
Result XmbOverlay::prepare(OverlayBounds bounds,OverlayCallback callback,OverlayInteraction interaction) {
  const bool passive=interaction==OverlayInteraction::passive;
  if(passive)callback={};
#ifdef _WIN32
  if(impl_->window&&!impl_->on_owner())return {false,"The overlay must be prepared on its owning UI thread."};
  const auto row=static_cast<std::size_t>(bounds.width)*4;
  if(!frame_bytes(bounds.width,bounds.height,row,frame_limit))return {false,"The overlay size is invalid or exceeds the supported memory limit."};
  impl_->release();impl_->owner=GetCurrentThreadId();impl_->bounds=bounds;impl_->monitor=monitor_of(bounds);impl_->callback=std::move(callback);impl_->stats={};impl_->passive=passive;
  if(const auto registered=Impl::register_class();!registered){impl_->release();return registered;}
  // WS_EX_NOACTIVATE keeps the game foreground; layered WS_EX_TRANSPARENT
  // windows are skipped by hit testing, so clicks reach the game.
  const DWORD style=WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TOPMOST|(passive?WS_EX_NOACTIVATE|WS_EX_TRANSPARENT:0);
  impl_->window=CreateWindowExW(style,focus_handoff::overlay_class,L"OpenXMB",WS_POPUP,
    bounds.x,bounds.y,static_cast<int>(bounds.width),static_cast<int>(bounds.height),nullptr,nullptr,GetModuleHandleW(nullptr),impl_.get());
  if(!impl_->window){auto failure=windows_failure("Windows could not create the overlay window");impl_->release();return failure;}
  if(auto allocated=make_surface(bounds.width,bounds.height,impl_->surface);!allocated){impl_->release();return allocated;}
  (void)conversion_table();return {true,{}};
#elif defined(__linux__)
  if(impl_->window&&!impl_->on_owner())return {false,"The overlay must be prepared on its owning UI thread."};
  if(!x11_detail::x11_session())return {false,"Transparent overlays require a native X11 session; Wayland needs compositor-specific support."};
  const auto row=static_cast<std::size_t>(bounds.width)*4;
  if(!frame_bytes(bounds.width,bounds.height,row,frame_limit))return {false,"The overlay size is invalid or exceeds the supported memory limit."};
  impl_->release();impl_->x=std::make_unique<x11_detail::Connection>();auto& x=*impl_->x;
  if(!x||!x.compositor()){impl_->release();return {false,"An active X11 compositing manager is required for transparent overlays."};}
  if(!passive&&!overlay_detectable_repeat(x)){impl_->release();return {false,"This X11 server cannot report reliable overlay press/release edges; use the normal XMB window."};}
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
  impl_->colormap=xcb_generate_id(x.c);impl_->window=xcb_generate_id(x.c);impl_->gc=xcb_generate_id(x.c);impl_->passive=passive;
  if(auto failure=x11_failure(x,xcb_create_colormap_checked(x.c,XCB_COLORMAP_ALLOC_NONE,impl_->colormap,x.screen->root,visual));!failure.empty()){impl_->release();return {false,"X11 could not allocate the overlay colormap"+failure+"."};}
  // The passive host is override-redirect so no window manager can focus it,
  // and it selects only Expose.
  const std::uint32_t values[]{0,0,XCB_EVENT_MASK_EXPOSURE|XCB_EVENT_MASK_KEY_PRESS|XCB_EVENT_MASK_KEY_RELEASE|XCB_EVENT_MASK_BUTTON_PRESS|XCB_EVENT_MASK_BUTTON_RELEASE|XCB_EVENT_MASK_POINTER_MOTION|XCB_EVENT_MASK_FOCUS_CHANGE,impl_->colormap};
  const std::uint32_t passive_values[]{0,0,1,XCB_EVENT_MASK_EXPOSURE,impl_->colormap};
  const auto created=passive?xcb_create_window_checked(x.c,32,impl_->window,x.screen->root,bounds.x,bounds.y,bounds.width,bounds.height,0,XCB_WINDOW_CLASS_INPUT_OUTPUT,visual,XCB_CW_BACK_PIXEL|XCB_CW_BORDER_PIXEL|XCB_CW_OVERRIDE_REDIRECT|XCB_CW_EVENT_MASK|XCB_CW_COLORMAP,passive_values):
    xcb_create_window_checked(x.c,32,impl_->window,x.screen->root,bounds.x,bounds.y,bounds.width,bounds.height,0,XCB_WINDOW_CLASS_INPUT_OUTPUT,visual,XCB_CW_BACK_PIXEL|XCB_CW_BORDER_PIXEL|XCB_CW_EVENT_MASK|XCB_CW_COLORMAP,values);
  if(auto failure=x11_failure(x,created);!failure.empty()){impl_->release();return {false,"X11 could not create the ARGB overlay window"+failure+"."};}
  if(auto failure=x11_failure(x,xcb_create_gc_checked(x.c,impl_->gc,impl_->window,0,nullptr));!failure.empty()){impl_->release();return {false,"X11 could not create the overlay graphics context"+failure+"."};}
  // Root StructureNotify on this private connection reports display geometry
  // changes; it does not redirect or alter other clients' root selections.
  const std::uint32_t root_mask[]{XCB_EVENT_MASK_STRUCTURE_NOTIFY};
  if(auto failure=x11_failure(x,xcb_change_window_attributes_checked(x.c,x.screen->root,XCB_CW_EVENT_MASK,root_mask));!failure.empty()){impl_->release();return {false,"X11 could not observe display geometry"+failure+"."};}
  {
    x11_detail::Reply<xcb_get_geometry_reply_t> root{xcb_get_geometry_reply(x.c,xcb_get_geometry(x.c,x.screen->root),nullptr)};
    if(!root){impl_->release();return {false,"X11 could not read the root window geometry."};}
    impl_->root_width=root->width;impl_->root_height=root->height;
  }
  const std::uint32_t pid[]{static_cast<std::uint32_t>(getpid())};
  if(passive) {
    const std::uint32_t type[]{x.atom("_NET_WM_WINDOW_TYPE_NOTIFICATION")};
    if(!x.property(impl_->window,"_NET_WM_WINDOW_TYPE",XCB_ATOM_ATOM,type)||!x.property(impl_->window,"_NET_WM_PID",XCB_ATOM_CARDINAL,pid)){impl_->release();return {false,"X11 could not configure the notification window."};}
    if(!overlay_empty_input(x,impl_->window)){impl_->release();return {false,"This X11 server cannot make the notification input-transparent (SHAPE 1.1 input regions are required)."};}
  } else {
    const std::uint32_t protocols[]{x.atom("WM_DELETE_WINDOW")},type[]{x.atom("_NET_WM_WINDOW_TYPE_UTILITY")},state[]{x.atom("_NET_WM_STATE_ABOVE")};
    if(!x.property(impl_->window,"WM_PROTOCOLS",XCB_ATOM_ATOM,protocols)||!x.property(impl_->window,"_NET_WM_WINDOW_TYPE",XCB_ATOM_ATOM,type)||!x.property(impl_->window,"_NET_WM_STATE",XCB_ATOM_ATOM,state)||!x.property(impl_->window,"_NET_WM_PID",XCB_ATOM_CARDINAL,pid)){impl_->release();return {false,"X11 could not configure the overlay window."};}
  }
  impl_->bounds=bounds;impl_->callback=std::move(callback);impl_->owner=std::this_thread::get_id();impl_->stats={};impl_->pixels.resize(row*bounds.height);(void)conversion_table();return {true,{}};
#else
  (void)bounds;(void)passive;return {false,"Transparent overlay is unavailable on this platform."};
#endif
}
Result XmbOverlay::resize(OverlayBounds bounds) {
  const auto row=static_cast<std::size_t>(bounds.width)*4;
  if(!frame_bytes(bounds.width,bounds.height,row,frame_limit))return {false,"The overlay size is invalid or exceeds the supported memory limit."};
#ifdef _WIN32
  if(!impl_->window||!impl_->on_owner())return {false,"The overlay is not prepared on this UI thread."};
  // The window keeps its old content and rectangle until the next present,
  // whose UpdateLayeredWindow moves and resizes it atomically with the frame.
  if(auto allocated=make_surface(bounds.width,bounds.height,impl_->surface);!allocated)return allocated;
  impl_->bounds=bounds;impl_->monitor=monitor_of(bounds);impl_->has_frame=false;return {true,{}};
#elif defined(__linux__)
  if(!impl_->window||!impl_->on_owner())return {false,"The overlay is not prepared on this UI thread."};
  auto& x=*impl_->x;
  const std::uint32_t values[]{static_cast<std::uint32_t>(bounds.x),static_cast<std::uint32_t>(bounds.y),bounds.width,bounds.height};
  if(auto failure=x11_failure(x,xcb_configure_window_checked(x.c,impl_->window,XCB_CONFIG_WINDOW_X|XCB_CONFIG_WINDOW_Y|XCB_CONFIG_WINDOW_WIDTH|XCB_CONFIG_WINDOW_HEIGHT,values));!failure.empty())
    return {false,"X11 could not resize the overlay"+failure+"."};
  impl_->pixels.resize(row*bounds.height);impl_->bounds=bounds;impl_->has_frame=false;impl_->display_dirty=false;return {true,{}};
#else
  return {false,"Transparent overlay is unavailable on this platform."};
#endif
}
Result XmbOverlay::present(std::span<const std::uint8_t> bgra,unsigned width,unsigned height,std::size_t stride) {
#ifdef _WIN32
  if(!impl_->window||!impl_->on_owner())return {false,"The overlay is not prepared on this UI thread."};
  if(width!=impl_->bounds.width||height!=impl_->bounds.height)return {false,size_mismatch(width,height,impl_->bounds)};
  const auto begin=std::chrono::steady_clock::now();const auto output_size=static_cast<std::size_t>(width)*height*4;
  const auto converted=convert_overlay_bgra(bgra,width,height,stride,{static_cast<std::uint8_t*>(impl_->surface.pixels),output_size},static_cast<std::size_t>(width)*4);
  if(!converted)return converted;
  const auto ready=std::chrono::steady_clock::now();POINT position{impl_->bounds.x,impl_->bounds.y},source{0,0};SIZE size{static_cast<LONG>(width),static_cast<LONG>(height)};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
  if(!UpdateLayeredWindow(impl_->window,nullptr,&position,&size,impl_->surface.dc,&source,0,&blend,ULW_ALPHA))return windows_failure("Windows could not present the transparent overlay (UpdateLayeredWindow)");
  if(IsWindowVisible(impl_->window)&&!SetWindowPos(impl_->window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE))
    return windows_failure("Windows could not keep the overlay above the application (SetWindowPos)");
  const auto end=std::chrono::steady_clock::now();++impl_->stats.frames;impl_->stats.conversion_ns+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(ready-begin).count());impl_->stats.present_ns+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end-ready).count());impl_->has_frame=true;return {true,{}};
#elif defined(__linux__)
  if(!impl_->window||!impl_->on_owner())return {false,"The overlay is not prepared on this UI thread."};
  auto& x=*impl_->x;impl_->pump();
  if(!x.compositor())return {false,"The X11 compositing manager is no longer available."};
  if(width!=impl_->bounds.width||height!=impl_->bounds.height)return {false,size_mismatch(width,height,impl_->bounds)};
  const auto begin=std::chrono::steady_clock::now();const auto row=static_cast<std::size_t>(width)*4;
  const auto converted=convert_overlay_bgra(bgra,width,height,stride,impl_->pixels,row);if(!converted)return converted;
  const auto ready=std::chrono::steady_clock::now();
  if(auto failure=impl_->upload();!failure.empty())return {false,"X11 could not present the transparent overlay"+failure+"."};
  if(impl_->passive&&impl_->mapped())impl_->raise();
  xcb_flush(x.c);const auto end=std::chrono::steady_clock::now();++impl_->stats.frames;impl_->stats.conversion_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(ready-begin).count();impl_->stats.present_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(end-ready).count();impl_->has_frame=true;return {true,{}};
#else
  (void)bgra;(void)width;(void)height;(void)stride;return {false,"Transparent overlay is unavailable on this platform."};
#endif
}
Result XmbOverlay::show(bool activate,const std::optional<SessionWindowTarget>& foreground_session) {
#ifdef _WIN32
  if(!impl_->window||!impl_->on_owner())return {false,"The overlay is not prepared on this UI thread."};
  if(!impl_->has_frame)return {false,"The overlay needs a rendered frame before it can be shown."};
  if(activate&&impl_->passive)return {false,"A passive notification host cannot take focus."};
  if(!SetWindowPos(impl_->window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW))return windows_failure("Windows did not show the overlay (SetWindowPos)");
  if(!IsWindowVisible(impl_->window))return {false,"Windows did not show the overlay: the window stayed hidden after SetWindowPos."};
  if(activate) {
    auto granted=activate_window(impl_->window,foreground_session);
    if(!granted){ShowWindow(impl_->window,SW_HIDE);return granted;}
    return granted;
  }
  return {true,{}};
#elif defined(__linux__)
  (void)foreground_session;
  if(!impl_->window||!impl_->on_owner())return {false,"The overlay is not prepared on this UI thread."};
  if(!impl_->has_frame)return {false,"The overlay needs a rendered frame before it can be shown."};
  if(activate&&impl_->passive)return {false,"A passive notification host cannot take focus."};
  auto& x=*impl_->x;if(!x.compositor())return {false,"The X11 compositing manager is no longer available."};
  if(auto failure=x11_failure(x,xcb_map_window_checked(x.c,impl_->window));!failure.empty())return {false,"X11 could not map the overlay"+failure+"."};
  if(activate&&!x.activate(impl_->window)){hide();return {false,"The window manager could not receive the _NET_ACTIVE_WINDOW request."};}
  for(unsigned tries=0;tries<20;++tries){impl_->pump();x11_detail::Reply<xcb_get_input_focus_reply_t> focus{xcb_get_input_focus_reply(x.c,xcb_get_input_focus(x.c),nullptr)};if(impl_->mapped()&&(!activate||(focus&&focus->focus==impl_->window&&x.active(impl_->window)))){if(impl_->passive)impl_->raise();if(auto failure=impl_->upload();!failure.empty()){hide();return {false,"The X11 overlay frame could not be restored after mapping"+failure+"."};}return {true,{}};}std::this_thread::sleep_for(std::chrono::milliseconds(5));}
  hide();return {false,activate?"The X11 window manager did not grant overlay focus within 100 ms of the _NET_ACTIVE_WINDOW request.":"The X11 overlay was not viewable within 100 ms of mapping."};
#else
  (void)activate;(void)foreground_session;return {false,"Transparent overlay is unavailable on this platform."};
#endif
}
bool XmbOverlay::display_changed()const{
#ifdef _WIN32
  if(!impl_->window||!impl_->on_owner())return false;
  const auto current=monitor_rect(MonitorFromWindow(impl_->window,MONITOR_DEFAULTTONEAREST));
  return current&&!EqualRect(&*current,&impl_->monitor);
#elif defined(__linux__)
  if(!impl_->window||!impl_->on_owner())return false;impl_->pump();return impl_->display_dirty;
#else
  return false;
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
