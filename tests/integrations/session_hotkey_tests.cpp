#include "openxmb/integrations/session_hotkey.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef __linux__
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#endif
using namespace openxmb::integrations;
using namespace std::chrono_literals;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
#ifdef __linux__
struct Fixture {
  Display* d{};Window window{};void* module{};
  using Fake=Bool(*)(Display*,unsigned,Bool,unsigned long);Fake fake{};
  XKeyboardState before{};Bool detectable_before{},supported{};
  Fixture(){require(std::getenv("OPENXMB_HOTKEY_TEST_ISOLATED"),"This input test requires an explicitly isolated Xvfb server");d=XOpenDisplay(nullptr);require(d,"isolated display missing");window=XCreateSimpleWindow(d,DefaultRootWindow(d),10,10,100,80,0,0,0x335577);XSelectInput(d,window,KeyPressMask|KeyReleaseMask);XMapWindow(d,window);XSetInputFocus(d,window,RevertToParent,CurrentTime);XSync(d,False);module=dlopen("libXtst.so.6",RTLD_NOW|RTLD_LOCAL);require(module,"test-only XTest runtime missing");fake=reinterpret_cast<Fake>(dlsym(module,"XTestFakeKeyEvent"));require(fake,"test-only XTest API missing");XGetKeyboardControl(d,&before);detectable_before=XkbGetDetectableAutoRepeat(d,&supported);}
  ~Fixture(){if(d){for(auto symbol:{XK_F1,XK_F2,XK_F3,XK_F4,XK_Control_L,XK_Shift_L})key(symbol,false);XDestroyWindow(d,window);XCloseDisplay(d);}if(module)dlclose(module);}
  void key(KeySym symbol,bool down){auto code=XKeysymToKeycode(d,symbol);require(code&&fake(d,code,down,0),"XTest fixture key failed");XSync(d,False);}
  void chord(bool down){if(down){key(XK_Control_L,true);key(XK_Shift_L,true);}else{key(XK_Shift_L,false);key(XK_Control_L,false);}}
  void tap(KeySym symbol){key(symbol,true);key(symbol,false);}
  void unchanged(){Window focus{};int revert{};XGetInputFocus(d,&focus,&revert);require(focus==window,"shortcut listener stole focus");XKeyboardState after{};XGetKeyboardControl(d,&after);require(after.global_auto_repeat==before.global_auto_repeat,"listener changed global autorepeat configuration");Bool available{};require(XkbGetDetectableAutoRepeat(d,&available)==detectable_before,"listener changed the parent's client autorepeat policy");}
};
unsigned drain(SessionHotkey& hotkey){unsigned result{};for(unsigned i=0;i<5;++i){std::this_thread::sleep_for(3ms);result+=hotkey.take_presses();}return result;}
#endif
int main()try{
  SessionHotkey hotkey;
#ifdef __linux__
  Fixture fixture;
  require(!hotkey.configure(0)&&!hotkey.configure(5),"invalid function-key number accepted");
  const auto configured=hotkey.configure(1);require(bool(configured),configured.message.c_str());fixture.unchanged();
  SessionHotkey conflict;require(!conflict.configure(1),"another client's passive grab conflict was ignored");
  fixture.chord(true);fixture.tap(XK_F1);require(drain(hotkey)==1,"background Ctrl+Shift+F1 was not delivered once");require(drain(conflict)==0,"failed registration retained delivery");
  fixture.key(XK_F1,true);require(drain(hotkey)==1,"new deliberate press was suppressed");
  std::this_thread::sleep_for(850ms);require(drain(hotkey)==0,"holding the shortcut generated repeated toggles");fixture.key(XK_F1,false);(void)drain(hotkey);
  for(unsigned i=0;i<8;++i)fixture.tap(XK_F1);require(drain(hotkey)==8,"queued rapid genuine presses were merged or lost");fixture.chord(false);
  fixture.chord(true);for(unsigned i=0;i<300;++i)fixture.tap(XK_F1);const auto first_batch=hotkey.take_presses();require(first_batch<=128,"one poll processed more than its bounded 256 native events");require(first_batch+drain(hotkey)==300,"bounded polling dropped queued deliberate presses");fixture.chord(false);
  for(unsigned locks=0;locks<4;++locks){if(locks&1)fixture.tap(XK_Caps_Lock);if(locks&2)fixture.tap(XK_Num_Lock);fixture.chord(true);fixture.tap(XK_F1);require(drain(hotkey)==1,"Caps/NumLock variant was not grabbed");fixture.chord(false);if(locks&1)fixture.tap(XK_Caps_Lock);if(locks&2)fixture.tap(XK_Num_Lock);}
  require(bool(hotkey.configure(2)),"reconfigure to F2 failed");require(bool(conflict.configure(1)),"old F1 grab was not released on reconfigure");fixture.chord(true);fixture.tap(XK_F1);fixture.tap(XK_F2);require(drain(hotkey)==1&&drain(conflict)==1,"reconfigured listeners received the wrong shortcut");fixture.chord(false);
  hotkey.stop();require(bool(conflict.configure(2)),"stop failed to release F2 grab");hotkey.stop();require(hotkey.take_presses()==0,"stopped listener replayed events");fixture.unchanged();
  const auto f4=XKeysymToKeycode(fixture.d,XK_F4);
  XGrabKey(fixture.d,f4,ControlMask|ShiftMask|LockMask,DefaultRootWindow(fixture.d),False,GrabModeAsync,GrabModeAsync);XSync(fixture.d,False);
  require(!hotkey.configure(4),"one conflicting lock variant did not fail the whole configuration");
  XUngrabKey(fixture.d,f4,ControlMask|ShiftMask|LockMask,DefaultRootWindow(fixture.d));XSync(fixture.d,False);
  {SessionHotkey rollback;require(bool(rollback.configure(4)),"partial grabs remained after a later lock variant failed");}
  require(bool(hotkey.configure(4)),"destructor failed to release passive grabs");fixture.chord(true);fixture.tap(XK_F4);hotkey.stop();require(bool(hotkey.configure(4))&&drain(hotkey)==0,"restart replayed a prior registration's queued tap");fixture.tap(XK_F4);require(drain(hotkey)==1,"F4 delivery failed after clean restart");fixture.chord(false);hotkey.stop();
  bool other_thread{};std::thread other([&]{other_thread=!conflict.configure(3)&&conflict.take_presses()==0;});other.join();require(other_thread,"listener accepted a non-owning thread");
  require(bool(hotkey.configure(3)),"F3 registration failed");fixture.chord(true);fixture.tap(XK_F3);require(drain(hotkey)==1,"F3 delivery failed");fixture.chord(false);
  setenv("XDG_SESSION_TYPE","wayland",1);require(!hotkey.configure(3),"Wayland silently registered an Xwayland shortcut");unsetenv("XDG_SESSION_TYPE");setenv("WAYLAND_DISPLAY","isolated-wayland-marker",1);require(!hotkey.configure(3),"Wayland environment silently registered an Xwayland shortcut");unsetenv("WAYLAND_DISPLAY");conflict.stop();fixture.unchanged();
  std::cout<<"Isolated X11 passive hotkey delivery, hold/rapid edges, locks, conflict, teardown and unchanged focus/input policy passed\n";
#else
  require(!hotkey.configure(1)&&hotkey.take_presses()==0,"unsupported platform reported a working X11 listener");hotkey.stop();std::cout<<"Unsupported-platform hotkey fallback passed\n";
#endif
  return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
