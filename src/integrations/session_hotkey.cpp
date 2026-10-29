#include "openxmb/integrations/session_hotkey.hpp"
#include <thread>
#ifdef __linux__
#include "x11_support.hpp"
#include <xcb/xcbext.h>
#include <X11/extensions/XKBproto.h>
#endif
namespace openxmb::integrations {
struct SessionHotkey::Impl {
  std::thread::id owner{std::this_thread::get_id()};
#ifdef __linux__
  std::unique_ptr<x11_detail::Connection> x;
  xcb_keycode_t key{};bool held{};
  void release()noexcept {
    if(x&&*x&&key)(void)x->check(xcb_ungrab_key_checked(x->c,key,x->screen->root,XCB_MOD_MASK_ANY));
    x.reset();key=0;held=false;
  }
#else
  void release()noexcept{}
#endif
  bool on_owner()const{return owner==std::this_thread::get_id();}
};
#ifdef __linux__
namespace {
xcb_extension_t xkb_extension{"XKEYBOARD",0};
template<class Reply,class Request>
x11_detail::Reply<Reply> xkb_request(x11_detail::Connection& x,unsigned opcode,Request request) {
  // Core XCB sends checked extension wire requests; this avoids adding a
  // libxcb-xkb dependency just to change one per-client protocol flag.
  const xcb_protocol_request_t protocol{2,&xkb_extension,static_cast<std::uint8_t>(opcode),0};
  iovec parts[4]{};parts[2].iov_base=&request;parts[2].iov_len=sizeof(request);parts[3].iov_len=(-sizeof(request))&3;
  const auto sequence=xcb_send_request(x.c,XCB_REQUEST_CHECKED,parts+2,&protocol);if(!sequence)return {};
  xcb_generic_error_t* error{};x11_detail::Reply<Reply> reply{static_cast<Reply*>(xcb_wait_for_reply(x.c,sequence,&error))};
  x11_detail::Reply<xcb_generic_error_t> release(error);return error?x11_detail::Reply<Reply>{}:std::move(reply);
}
bool detectable_repeat(x11_detail::Connection& x) {
  const auto* extension=xcb_get_extension_data(x.c,&xkb_extension);if(!extension||!extension->present)return false;
  xkbUseExtensionReq use{};use.wantedMajor=1;use.wantedMinor=0;
  const auto version=xkb_request<xkbUseExtensionReply>(x,X_kbUseExtension,use);if(!version||!version->supported)return false;
  xkbPerClientFlagsReq request{};request.deviceSpec=XkbUseCoreKbd;request.change=XkbPCF_DetectableAutoRepeatMask;request.value=XkbPCF_DetectableAutoRepeatMask;
  const auto flags=xkb_request<xkbPerClientFlagsReply>(x,X_kbPerClientFlags,request);
  return flags&&(flags->supported&XkbPCF_DetectableAutoRepeatMask)&&(flags->value&XkbPCF_DetectableAutoRepeatMask);
}
}
#endif
SessionHotkey::SessionHotkey():impl_(std::make_unique<Impl>()){}
SessionHotkey::~SessionHotkey(){stop();}
Result SessionHotkey::configure(unsigned key){
  if(!impl_->on_owner())return {false,"Configure the session shortcut on its owning UI thread."};
  impl_->release();
  if(key<1||key>4)return {false,"Choose a session shortcut from Ctrl+Shift+F1 through Ctrl+Shift+F4."};
#ifdef __linux__
  auto connection=std::make_unique<x11_detail::Connection>();
  if(!*connection)return {false,"Global session shortcuts require a native X11 session; Wayland/Xwayland are unavailable."};
  auto& x=*connection;
  if(!detectable_repeat(x))return {false,"This X11 server cannot report reliable shortcut press/release edges."};
  const auto* setup=xcb_get_setup(x.c);const unsigned count=unsigned(setup->max_keycode)-setup->min_keycode+1;
  if(!count||count>255)return {false,"The X11 keyboard map is unavailable."};
  x11_detail::Reply<xcb_get_keyboard_mapping_reply_t> mapping{xcb_get_keyboard_mapping_reply(x.c,xcb_get_keyboard_mapping(x.c,setup->min_keycode,count),nullptr)};
  x11_detail::Reply<xcb_get_modifier_mapping_reply_t> modifiers{xcb_get_modifier_mapping_reply(x.c,xcb_get_modifier_mapping(x.c),nullptr)};
  if(!mapping||!mapping->keysyms_per_keycode||!modifiers||xcb_get_keyboard_mapping_keysyms_length(mapping.get())!=static_cast<int>(count*mapping->keysyms_per_keycode))return {false,"The X11 keyboard/modifier map is unavailable."};
  const auto* symbols=xcb_get_keyboard_mapping_keysyms(mapping.get());const auto* modifier_keys=xcb_get_modifier_mapping_keycodes(modifiers.get());
  unsigned ignored=XCB_MOD_MASK_LOCK;xcb_keycode_t function{};
  for(unsigned index=0;index<count;++index){bool lock=false;for(unsigned column=0;column<mapping->keysyms_per_keycode;++column){const auto symbol=symbols[index*mapping->keysyms_per_keycode+column];if(!function&&symbol==0xffbe + key - 1)function=setup->min_keycode+index;if(symbol==0xff7f||symbol==0xffe5)lock=true;}
    if(lock)for(unsigned modifier=0;modifier<8;++modifier)for(unsigned slot=0;slot<modifiers->keycodes_per_modifier;++slot)if(modifier_keys[modifier*modifiers->keycodes_per_modifier+slot]==setup->min_keycode+index)ignored|=1u<<modifier;
  }
  if(!function)return {false,"The selected function key is not present in the X11 keyboard map."};
  ignored&=~(XCB_MOD_MASK_CONTROL|XCB_MOD_MASK_SHIFT);
  impl_->x=std::move(connection);impl_->key=function;
  for(unsigned subset=ignored;;subset=(subset-1)&ignored){
    const auto mask=XCB_MOD_MASK_CONTROL|XCB_MOD_MASK_SHIFT|subset;
    if(!x.check(xcb_grab_key_checked(x.c,0,x.screen->root,mask,function,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC))){impl_->release();return {false,"The X11 session shortcut is already in use or keyboard grabs are unavailable. Choose another shortcut."};}
    if(!subset)break;
  }
  return {true,"X11 session shortcut registered."};
#else
  return {false,"Global session shortcut is unavailable on this platform."};
#endif
}
void SessionHotkey::stop()noexcept{if(impl_&&impl_->on_owner())impl_->release();}
unsigned SessionHotkey::take_presses(){
  if(!impl_->on_owner())return 0;
#ifdef __linux__
  if(!impl_->x||!*impl_->x){impl_->release();return 0;}
  unsigned presses{};
  for(unsigned count=0;count<256;++count){x11_detail::Reply<xcb_generic_event_t> event{xcb_poll_for_event(impl_->x->c)};if(!event)break;
    const auto kind=event->response_type&0x7f;
    if(kind!=XCB_KEY_PRESS&&kind!=XCB_KEY_RELEASE)continue;
    const auto* key=reinterpret_cast<const xcb_key_press_event_t*>(event.get());if(key->detail!=impl_->key)continue;
    if(kind==XCB_KEY_RELEASE)impl_->held=false;
    else if(!impl_->held){impl_->held=true;++presses;}
  }
  return presses;
#else
  return 0;
#endif
}
}
